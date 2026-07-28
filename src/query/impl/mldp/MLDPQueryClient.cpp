//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/MLDPQueryClient.h>

#include <pool/MLDPGrpcQueryPoolConfig.h>

#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/QueryResult.h>
#include <query/executor/ExecutorUtils.h>

#include <util/log/Logger.h>

#include <google/protobuf/message.h>
#include <grpcpp/grpcpp.h>
#include <query.grpc.pb.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_nested.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/builder_union.h>
#include <arrow/builder.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <arrow/util/key_value_metadata.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::log;
using mldp_pvxs_driver::util::pool::MLDPGrpcQueryPool;

const std::set<std::string_view> MLDPQueryClient::kVirtualTables = {
    "mldp.time_series",
    "mldp.time_series_table",
    "mldp.pv_stats",
};

std::set<std::string_view> MLDPQueryClient::virtualTables() const
{
    return kVirtualTables;
}

std::vector<ColumnSchema> MLDPQueryClient::tableSchema(std::string_view table_name) const
{
    if (table_name == "mldp.time_series" || table_name == "mldp.time_series_table")
    {
        const bool wide_table = table_name == "mldp.time_series_table";
        std::vector<ColumnSchema> schema = {
            {"pv", ColumnType::STRING, true, !wide_table, {PredicateOp::EQ, PredicateOp::IN}, {}, "Source name"},
            {"time", ColumnType::TIMESTAMP, false, true, {PredicateOp::GTE, PredicateOp::LTE}, {}, "Sample timestamp"},
            {"value", ColumnType::NATIVE_VALUE, false, !wide_table, {}, {PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::LT, PredicateOp::LTE, PredicateOp::GT, PredicateOp::GTE, PredicateOp::IN, PredicateOp::BETWEEN}, "Native sample value"},
            {"column_type", ColumnType::STRING, false, !wide_table, {PredicateOp::EQ, PredicateOp::IN}, {PredicateOp::EQ, PredicateOp::IN}, "Native MLDP data-value type"},
            {"tags", ColumnType::STRING, false, !wide_table, {}, {}, "Bucket column-metadata tag collection; filter with tag = or tag IN locally"},
            {"attributes", ColumnType::STRING, false, !wide_table, {}, {}, "Bucket column-metadata dynamic attribute map; select/filter attributes.<key> locally"},
            {"provenance", ColumnType::STRING, false, !wide_table, {}, {}, "Bucket column-metadata dynamic provenance map; select/filter provenance.<key> locally"},
            {"tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {PredicateOp::EQ, PredicateOp::IN}, "Tag membership predicate shorthand for tags"},
            {"timeout", ColumnType::DURATION_SECONDS, false, false, {PredicateOp::EQ}, {}, "Query timeout"},
            {"rpc_deadline", ColumnType::DURATION_SECONDS, false, false, {PredicateOp::EQ}, {}, "RPC deadline"}};
        if (wide_table)
        {
            schema.emplace_back("window", ColumnType::TIMESTAMP, false, false, std::set<PredicateOp>{PredicateOp::IN}, std::set<PredicateOp>{},
                                "Wide-table interval input; accepts window IN (start, end) or window IN (SELECT time, end_time ...)");
        }
        return schema;
    }
    if (table_name == "mldp.pv_stats")
    {
        return {{"pv", ColumnType::STRING, true, true, {PredicateOp::EQ, PredicateOp::IN}, {}, "Source name"},
                {"first_timestamp", ColumnType::TIMESTAMP, false, true, {}, {}, "First observed timestamp"},
                {"last_timestamp", ColumnType::TIMESTAMP, false, true, {}, {}, "Last observed timestamp"},
                {"num_buckets", ColumnType::INT, false, true, {}, {}, "Number of buckets"}};
    }
    throw std::invalid_argument("MLDPQueryClient: unknown virtual table: " + std::string(table_name));
}

namespace {

constexpr std::string_view kTimeSeriesTable = "mldp.time_series";
constexpr std::string_view kTimeSeriesWideTable = "mldp.time_series_table";
constexpr std::string_view kPvStatsTable = "mldp.pv_stats";

int64_t timestampToNanoseconds(const dp::service::common::Timestamp& timestamp)
{
    return static_cast<int64_t>(timestamp.epochseconds()) * 1'000'000'000LL +
           static_cast<int64_t>(timestamp.nanoseconds());
}

void setTimestamp(dp::service::common::Timestamp* target, int64_t seconds)
{
    target->set_epochseconds(static_cast<uint64_t>(seconds));
    target->set_nanoseconds(0);
}

std::vector<std::string> requestedPvs(const std::vector<Predicate>& predicates)
{
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const auto& predicate : predicates)
    {
        if (predicate.column != "pv" || (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN))
            continue;
        for (const auto& value : predicate.values)
        {
            if (!std::holds_alternative<std::string>(value))
                throw std::invalid_argument("MLDP query predicate pv requires string values");
            const auto& name = std::get<std::string>(value);
            if (seen.insert(name).second)
                names.push_back(name);
        }
    }
    if (names.empty())
        throw std::invalid_argument("MLDP query requires an explicit pv = or pv IN predicate");
    return names;
}

std::string_view dataValueKind(const dp::service::common::DataValue& value)
{
    switch (value.value_case())
    {
    case dp::service::common::DataValue::kStringValue: return "string";
    case dp::service::common::DataValue::kBooleanValue: return "bool";
    case dp::service::common::DataValue::kUintValue: return "uint32";
    case dp::service::common::DataValue::kUlongValue: return "uint64";
    case dp::service::common::DataValue::kIntValue: return "int32";
    case dp::service::common::DataValue::kLongValue: return "int64";
    case dp::service::common::DataValue::kFloatValue: return "float";
    case dp::service::common::DataValue::kDoubleValue: return "double";
    case dp::service::common::DataValue::kByteArrayValue: return "binary";
    case dp::service::common::DataValue::kTimestampValue: return "timestamp";
    case dp::service::common::DataValue::kArrayValue: return "array";
    case dp::service::common::DataValue::kStructureValue: return "structure";
    case dp::service::common::DataValue::kImageValue: return "image";
    case dp::service::common::DataValue::VALUE_NOT_SET: return "null";
    }
    return "null";
}

std::optional<std::string_view> columnValueKind(const dp::service::common::DataColumn& column)
{
    std::optional<std::string_view> kind;
    for (const auto& value : column.datavalues())
    {
        if (value.value_case() == dp::service::common::DataValue::VALUE_NOT_SET)
            continue;
        const auto candidate = dataValueKind(value);
        if (!kind)
            kind = candidate;
        else if (*kind != candidate)
            throw std::runtime_error("MLDP queryTable PV column '" + column.name() + "' contains mixed data types");
    }
    return kind;
}

bool matchesStringPredicate(const Predicate& predicate, const std::string_view value)
{
    if (predicate.op == PredicateOp::PREFIX || predicate.op == PredicateOp::CONTAINS || predicate.op == PredicateOp::LIKE)
    {
        if (predicate.values.size() != 1 || !std::holds_alternative<std::string>(predicate.values.front()))
            throw std::invalid_argument("MLDP metadata predicate '" + predicate.column + "' requires one string value");
        const auto& pattern = std::get<std::string>(predicate.values.front());
        if (predicate.op == PredicateOp::PREFIX)
            return value.rfind(pattern, 0) == 0;
        if (predicate.op == PredicateOp::CONTAINS)
            return value.find(pattern) != std::string_view::npos;
        return executor::matchesLikePattern(value, pattern);
    }
    if (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)
        throw std::invalid_argument("MLDP metadata predicate '" + predicate.column + "' requires =, IN, PREFIX, CONTAINS, or LIKE");
    for (const auto& candidate : predicate.values)
    {
        if (!std::holds_alternative<std::string>(candidate))
            throw std::invalid_argument("MLDP metadata predicate '" + predicate.column + "' requires string values");
        if (std::get<std::string>(candidate) == value)
            return true;
    }
    return false;
}

bool matchesColumnPredicates(const dp::service::common::DataColumn& column,
                             const std::vector<Predicate>&           predicates)
{
    for (const auto& predicate : predicates)
    {
        if (predicate.column == "column_type")
        {
            const auto kind = columnValueKind(column);
            if (!kind || !matchesStringPredicate(predicate, *kind))
                return false;
        }
        else if (predicate.column == "tag")
        {
            bool matched = false;
            for (const auto& tag : column.metadata().tags())
            {
                if (matchesStringPredicate(predicate, tag))
                {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        else if (predicate.column.rfind("attributes.", 0) == 0)
        {
            const auto key = predicate.column.substr(std::string("attributes.").size());
            bool matched = false;
            for (const auto& attribute : column.metadata().attributes())
            {
                if (attribute.name() == key && matchesStringPredicate(predicate, attribute.value()))
                {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        else if (predicate.column.rfind("provenance.", 0) == 0)
        {
            const auto key = predicate.column.substr(std::string("provenance.").size());
            const auto& provenance = column.metadata().provenance();
            const std::string value = key == "source" ? provenance.source() : key == "process" ? provenance.process() : "";
            if (value.empty() || !matchesStringPredicate(predicate, value))
                return false;
        }
    }
    return true;
}

std::shared_ptr<arrow::DataType> dataValueArrowType(const dp::service::common::DataValue& value)
{
    switch (value.value_case())
    {
    case dp::service::common::DataValue::kStringValue: return arrow::utf8();
    case dp::service::common::DataValue::kBooleanValue: return arrow::boolean();
    case dp::service::common::DataValue::kUintValue: return arrow::uint32();
    case dp::service::common::DataValue::kUlongValue: return arrow::uint64();
    case dp::service::common::DataValue::kIntValue: return arrow::int32();
    case dp::service::common::DataValue::kLongValue: return arrow::int64();
    case dp::service::common::DataValue::kFloatValue: return arrow::float32();
    case dp::service::common::DataValue::kDoubleValue: return arrow::float64();
    case dp::service::common::DataValue::kByteArrayValue:
    case dp::service::common::DataValue::kArrayValue:
    case dp::service::common::DataValue::kStructureValue:
    case dp::service::common::DataValue::kImageValue: return arrow::binary();
    case dp::service::common::DataValue::kTimestampValue: return arrow::timestamp(arrow::TimeUnit::NANO, "UTC");
    case dp::service::common::DataValue::VALUE_NOT_SET: return arrow::null();
    }
    return arrow::null();
}

void appendNativeValue(arrow::ArrayBuilder& builder, const dp::service::common::DataValue& value)
{
    const auto append_serialized = [&value](arrow::BinaryBuilder& binary, const google::protobuf::Message& message)
    {
        if (!binary.Append(message.SerializeAsString()).ok())
            throw std::runtime_error("Failed to append serialized MLDP data value");
    };
    if (value.value_case() == dp::service::common::DataValue::VALUE_NOT_SET)
    {
        if (!builder.AppendNull().ok()) throw std::runtime_error("Failed to append null MLDP data value");
        return;
    }
    switch (value.value_case())
    {
    case dp::service::common::DataValue::kStringValue: if (!dynamic_cast<arrow::StringBuilder&>(builder).Append(value.stringvalue()).ok()) throw std::runtime_error("Failed to append string"); break;
    case dp::service::common::DataValue::kBooleanValue: if (!dynamic_cast<arrow::BooleanBuilder&>(builder).Append(value.booleanvalue()).ok()) throw std::runtime_error("Failed to append bool"); break;
    case dp::service::common::DataValue::kUintValue: if (!dynamic_cast<arrow::UInt32Builder&>(builder).Append(value.uintvalue()).ok()) throw std::runtime_error("Failed to append uint32"); break;
    case dp::service::common::DataValue::kUlongValue: if (!dynamic_cast<arrow::UInt64Builder&>(builder).Append(value.ulongvalue()).ok()) throw std::runtime_error("Failed to append uint64"); break;
    case dp::service::common::DataValue::kIntValue: if (!dynamic_cast<arrow::Int32Builder&>(builder).Append(value.intvalue()).ok()) throw std::runtime_error("Failed to append int32"); break;
    case dp::service::common::DataValue::kLongValue: if (!dynamic_cast<arrow::Int64Builder&>(builder).Append(value.longvalue()).ok()) throw std::runtime_error("Failed to append int64"); break;
    case dp::service::common::DataValue::kFloatValue: if (!dynamic_cast<arrow::FloatBuilder&>(builder).Append(value.floatvalue()).ok()) throw std::runtime_error("Failed to append float"); break;
    case dp::service::common::DataValue::kDoubleValue: if (!dynamic_cast<arrow::DoubleBuilder&>(builder).Append(value.doublevalue()).ok()) throw std::runtime_error("Failed to append double"); break;
    case dp::service::common::DataValue::kByteArrayValue: if (!dynamic_cast<arrow::BinaryBuilder&>(builder).Append(value.bytearrayvalue()).ok()) throw std::runtime_error("Failed to append binary"); break;
    case dp::service::common::DataValue::kTimestampValue: if (!dynamic_cast<arrow::TimestampBuilder&>(builder).Append(timestampToNanoseconds(value.timestampvalue())).ok()) throw std::runtime_error("Failed to append timestamp"); break;
    case dp::service::common::DataValue::kArrayValue: append_serialized(dynamic_cast<arrow::BinaryBuilder&>(builder), value.arrayvalue()); break;
    case dp::service::common::DataValue::kStructureValue: append_serialized(dynamic_cast<arrow::BinaryBuilder&>(builder), value.structurevalue()); break;
    case dp::service::common::DataValue::kImageValue: append_serialized(dynamic_cast<arrow::BinaryBuilder&>(builder), value.imagevalue()); break;
    case dp::service::common::DataValue::VALUE_NOT_SET: break;
    }
}

std::pair<int64_t, int64_t> requestedTimeRange(const std::vector<Predicate>& predicates)
{
    int64_t begin = 0;
    int64_t end = std::numeric_limits<int64_t>::max() / 1'000'000'000LL;
    for (const auto& predicate : predicates)
    {
        if (predicate.column != "time")
            continue;
        if ((predicate.op != PredicateOp::GTE && predicate.op != PredicateOp::LTE) || predicate.values.size() != 1 ||
            !std::holds_alternative<int64_t>(predicate.values.front()))
            throw std::invalid_argument("MLDP time predicate must be time >= or time <= with an integer timestamp");
        if (predicate.op == PredicateOp::GTE)
            begin = std::get<int64_t>(predicate.values.front());
        else
            end = std::get<int64_t>(predicate.values.front());
    }
    return {begin, end};
}

std::set<std::string> attributeKeys(const std::vector<dp::service::common::ColumnMetadata>& metadata)
{
    std::set<std::string> keys;
    for (const auto& column_metadata : metadata)
    {
        for (const auto& attribute : column_metadata.attributes())
        {
            keys.insert(attribute.name());
        }
    }
    return keys;
}

std::set<std::string> provenanceKeys(const std::vector<dp::service::common::ColumnMetadata>& metadata)
{
    std::set<std::string> keys;
    for (const auto& column_metadata : metadata)
    {
        if (!column_metadata.provenance().source().empty())
        {
            keys.insert("source");
        }
        if (!column_metadata.provenance().process().empty())
        {
            keys.insert("process");
        }
    }
    return keys;
}

std::set<std::string> requestedDynamicMetadataKeys(const std::set<std::string>& projection_hint, const std::string_view prefix)
{
    std::set<std::string> keys;
    for (const auto& column : projection_hint)
    {
        if (column.rfind(prefix, 0) == 0 && column.size() > prefix.size())
        {
            keys.insert(column.substr(prefix.size()));
        }
    }
    return keys;
}

void addRequestedDynamicMetadataKeys(std::set<std::string>&       keys,
                                     const std::set<std::string>& projection_hint,
                                     const std::string_view        prefix)
{
    const auto requested = requestedDynamicMetadataKeys(projection_hint, prefix);
    keys.insert(requested.begin(), requested.end());
}

std::shared_ptr<arrow::KeyValueMetadata> arrowFieldMetadata(const dp::service::common::ColumnMetadata& metadata)
{
    std::vector<std::string> keys;
    std::vector<std::string> values;
    if (metadata.tags_size() > 0)
    {
        std::string tags;
        for (const auto& tag : metadata.tags())
        {
            if (!tags.empty())
                tags += ',';
            tags += tag;
        }
        keys.emplace_back("tags");
        values.push_back(std::move(tags));
    }
    for (const auto& attribute : metadata.attributes())
    {
        keys.push_back("attributes." + attribute.name());
        values.push_back(attribute.value());
    }
    if (!metadata.provenance().source().empty())
    {
        keys.emplace_back("provenance.source");
        values.push_back(metadata.provenance().source());
    }
    if (!metadata.provenance().process().empty())
    {
        keys.emplace_back("provenance.process");
        values.push_back(metadata.provenance().process());
    }
    return keys.empty() ? nullptr : std::make_shared<arrow::KeyValueMetadata>(std::move(keys), std::move(values));
}

class TimeSeriesMetadataBuilders
{
public:
    TimeSeriesMetadataBuilders(const std::set<std::string>& attributes, const std::set<std::string>& provenance)
        : tags_values_(std::make_shared<arrow::StringBuilder>()), tags_(arrow::default_memory_pool(), tags_values_), attributes_keys_(std::make_shared<arrow::StringBuilder>()), attributes_values_(std::make_shared<arrow::StringBuilder>()), attributes_(arrow::default_memory_pool(), attributes_keys_, attributes_values_), provenance_keys_(std::make_shared<arrow::StringBuilder>()), provenance_values_(std::make_shared<arrow::StringBuilder>()), provenance_(arrow::default_memory_pool(), provenance_keys_, provenance_values_)
    {
        for (const auto& key : attributes)
            attributes_values_by_key_.emplace(key, std::make_unique<arrow::StringBuilder>());
        for (const auto& key : provenance)
            provenance_values_by_key_.emplace(key, std::make_unique<arrow::StringBuilder>());
    }

    void append(const dp::service::common::ColumnMetadata& metadata)
    {
        if (!tags_.Append().ok() || !attributes_.Append().ok() || !provenance_.Append().ok())
            throw std::runtime_error("Failed to begin Arrow time-series metadata collection");
        for (const auto& tag : metadata.tags())
            append(*tags_values_, tag);
        std::map<std::string, std::string> attributes;
        for (const auto& attribute : metadata.attributes())
        {
            attributes[attribute.name()] = attribute.value();
            append(*attributes_keys_, attribute.name());
            append(*attributes_values_, attribute.value());
        }
        std::map<std::string, std::string> provenance;
        if (!metadata.provenance().source().empty())
        {
            provenance.emplace("source", metadata.provenance().source());
        }
        if (!metadata.provenance().process().empty())
        {
            provenance.emplace("process", metadata.provenance().process());
        }
        for (const auto& [key, value] : provenance)
        {
            append(*provenance_keys_, key);
            append(*provenance_values_, value);
        }
        appendScalars(attributes_values_by_key_, attributes);
        appendScalars(provenance_values_by_key_, provenance);
    }

    void finish(std::vector<std::shared_ptr<arrow::Field>>& fields, std::vector<std::shared_ptr<arrow::Array>>& arrays)
    {
        finishCollection("tags", tags_, fields, arrays);
        finishCollection("attributes", attributes_, fields, arrays);
        finishCollection("provenance", provenance_, fields, arrays);
        finishScalars("attributes.", attributes_values_by_key_, fields, arrays);
        finishScalars("provenance.", provenance_values_by_key_, fields, arrays);
    }

private:
    static void append(arrow::StringBuilder& builder, std::string_view value)
    {
        if (!builder.Append(value).ok())
            throw std::runtime_error("Failed to append Arrow metadata string");
    }

    static void appendScalars(std::map<std::string, std::unique_ptr<arrow::StringBuilder>>& builders,
                              const std::map<std::string, std::string>&                     values)
    {
        for (auto& [key, builder] : builders)
            if (const auto it = values.find(key); it != values.end())
                append(*builder, it->second);
            else if (!builder->AppendNull().ok())
                throw std::runtime_error("Failed to append null metadata scalar");
    }

    template <typename Builder>
    static void finishCollection(const std::string& name, Builder& builder, std::vector<std::shared_ptr<arrow::Field>>& fields, std::vector<std::shared_ptr<arrow::Array>>& arrays)
    {
        std::shared_ptr<arrow::Array> array;
        if (!builder.Finish(&array).ok())
            throw std::runtime_error("Failed to finish Arrow metadata collection");
        fields.push_back(arrow::field(name, array->type()));
        arrays.push_back(std::move(array));
    }

    static void finishScalars(const std::string& prefix, std::map<std::string, std::unique_ptr<arrow::StringBuilder>>& builders, std::vector<std::shared_ptr<arrow::Field>>& fields, std::vector<std::shared_ptr<arrow::Array>>& arrays)
    {
        for (auto& [key, builder] : builders)
        {
            std::shared_ptr<arrow::Array> array;
            if (!builder->Finish(&array).ok())
                throw std::runtime_error("Failed to finish Arrow metadata scalar");
            fields.push_back(arrow::field(prefix + key, array->type(), true));
            arrays.push_back(std::move(array));
        }
    }

    std::shared_ptr<arrow::StringBuilder>                        tags_values_;
    arrow::ListBuilder                                           tags_;
    std::shared_ptr<arrow::StringBuilder>                        attributes_keys_, attributes_values_;
    arrow::MapBuilder                                            attributes_;
    std::shared_ptr<arrow::StringBuilder>                        provenance_keys_, provenance_values_;
    arrow::MapBuilder                                            provenance_;
    std::map<std::string, std::unique_ptr<arrow::StringBuilder>> attributes_values_by_key_, provenance_values_by_key_;
};

std::shared_ptr<arrow::DataType> dataValueType()
{
    return arrow::dense_union({arrow::field("string", arrow::utf8()),
                               arrow::field("bool", arrow::boolean()),
                               arrow::field("uint32", arrow::uint32()),
                               arrow::field("uint64", arrow::uint64()),
                               arrow::field("int32", arrow::int32()),
                               arrow::field("int64", arrow::int64()),
                               arrow::field("float", arrow::float32()),
                               arrow::field("double", arrow::float64()),
                               arrow::field("binary", arrow::binary()),
                               arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO, "UTC")),
                               arrow::field("array", arrow::binary()),
                               arrow::field("structure", arrow::binary()),
                               arrow::field("image", arrow::binary())});
}

class DataValueBuilder
{
public:
    explicit DataValueBuilder(arrow::MemoryPool* pool)
        : string_(std::make_shared<arrow::StringBuilder>(pool))
        , boolean_(std::make_shared<arrow::BooleanBuilder>(pool))
        , uint32_(std::make_shared<arrow::UInt32Builder>(pool))
        , uint64_(std::make_shared<arrow::UInt64Builder>(pool))
        , int32_(std::make_shared<arrow::Int32Builder>(pool))
        , int64_(std::make_shared<arrow::Int64Builder>(pool))
        , float_(std::make_shared<arrow::FloatBuilder>(pool))
        , double_(std::make_shared<arrow::DoubleBuilder>(pool))
        , binary_(std::make_shared<arrow::BinaryBuilder>(pool))
        , timestamp_(std::make_shared<arrow::TimestampBuilder>(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool))
        , array_(std::make_shared<arrow::BinaryBuilder>(pool))
        , structure_(std::make_shared<arrow::BinaryBuilder>(pool))
        , image_(std::make_shared<arrow::BinaryBuilder>(pool))
        , union_builder_(pool,
                         {string_, boolean_, uint32_, uint64_, int32_, int64_, float_, double_, binary_, timestamp_, array_, structure_, image_},
                         dataValueType())
    {
    }

    void append(const dp::service::common::DataValue& value)
    {
        const auto appendSerialized = [&](int8_t type_id, arrow::BinaryBuilder& builder, const google::protobuf::Message& message)
        {
            const auto serialized = message.SerializeAsString();
            if (!union_builder_.Append(type_id).ok() || !builder.Append(serialized).ok())
                throw std::runtime_error("Failed to append Arrow DataValue union member");
        };
        switch (value.value_case())
        {
        case dp::service::common::DataValue::kStringValue: append(0, *string_, value.stringvalue()); break;
        case dp::service::common::DataValue::kBooleanValue: append(1, *boolean_, value.booleanvalue()); break;
        case dp::service::common::DataValue::kUintValue: append(2, *uint32_, value.uintvalue()); break;
        case dp::service::common::DataValue::kUlongValue: append(3, *uint64_, value.ulongvalue()); break;
        case dp::service::common::DataValue::kIntValue: append(4, *int32_, value.intvalue()); break;
        case dp::service::common::DataValue::kLongValue: append(5, *int64_, value.longvalue()); break;
        case dp::service::common::DataValue::kFloatValue: append(6, *float_, value.floatvalue()); break;
        case dp::service::common::DataValue::kDoubleValue: append(7, *double_, value.doublevalue()); break;
        case dp::service::common::DataValue::kByteArrayValue: append(8, *binary_, value.bytearrayvalue()); break;
        case dp::service::common::DataValue::kTimestampValue: append(9, *timestamp_, timestampToNanoseconds(value.timestampvalue())); break;
        case dp::service::common::DataValue::kArrayValue: appendSerialized(10, *array_, value.arrayvalue()); break;
        case dp::service::common::DataValue::kStructureValue: appendSerialized(11, *structure_, value.structurevalue()); break;
        case dp::service::common::DataValue::kImageValue: appendSerialized(12, *image_, value.imagevalue()); break;
        case dp::service::common::DataValue::VALUE_NOT_SET:
            if (!union_builder_.AppendNull().ok())
                throw std::runtime_error("Failed to append null Arrow DataValue");
            break;
        }
    }

    std::shared_ptr<arrow::Array> finish()
    {
        std::shared_ptr<arrow::Array> array;
        if (!union_builder_.Finish(&array).ok())
            throw std::runtime_error("Failed to finish Arrow DataValue union");
        return array;
    }

private:
    template <typename Builder, typename Value>
    void append(int8_t type_id, Builder& builder, const Value& value)
    {
        if (!union_builder_.Append(type_id).ok() || !builder.Append(value).ok())
            throw std::runtime_error("Failed to append Arrow DataValue union member");
    }

    std::shared_ptr<arrow::StringBuilder>    string_;
    std::shared_ptr<arrow::BooleanBuilder>   boolean_;
    std::shared_ptr<arrow::UInt32Builder>    uint32_;
    std::shared_ptr<arrow::UInt64Builder>    uint64_;
    std::shared_ptr<arrow::Int32Builder>     int32_;
    std::shared_ptr<arrow::Int64Builder>     int64_;
    std::shared_ptr<arrow::FloatBuilder>     float_;
    std::shared_ptr<arrow::DoubleBuilder>    double_;
    std::shared_ptr<arrow::BinaryBuilder>    binary_;
    std::shared_ptr<arrow::TimestampBuilder> timestamp_;
    std::shared_ptr<arrow::BinaryBuilder>    array_;
    std::shared_ptr<arrow::BinaryBuilder>    structure_;
    std::shared_ptr<arrow::BinaryBuilder>    image_;
    arrow::DenseUnionBuilder                 union_builder_;
};

std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeQueryClientLogger()
{
    std::string name = "mldp_query_client";
    return mldp_pvxs_driver::util::log::newLogger(name);
}

using dp::service::common::DataTimestamps;
using dp::service::common::Timestamp;

SourceTimestamp makeSourceTimestamp(const Timestamp& ts)
{
    return SourceTimestamp{ts.epochseconds(), ts.nanoseconds()};
}

bool isBefore(const SourceTimestamp& lhs, const SourceTimestamp& rhs)
{
    if (lhs.epoch_seconds != rhs.epoch_seconds)
    {
        return lhs.epoch_seconds < rhs.epoch_seconds;
    }
    return lhs.nanoseconds < rhs.nanoseconds;
}

std::optional<std::pair<SourceTimestamp, SourceTimestamp>>
extractTimestampRange(const DataTimestamps& data_timestamps)
{
    if (data_timestamps.has_timestamplist())
    {
        const auto& list = data_timestamps.timestamplist();
        if (list.timestamps_size() <= 0)
        {
            return std::nullopt;
        }
        SourceTimestamp first = makeSourceTimestamp(list.timestamps(0));
        SourceTimestamp last = first;
        for (int i = 1; i < list.timestamps_size(); ++i)
        {
            const SourceTimestamp current = makeSourceTimestamp(list.timestamps(i));
            if (isBefore(current, first))
                first = current;
            if (isBefore(last, current))
                last = current;
        }
        return std::make_pair(first, last);
    }
    if (data_timestamps.has_samplingclock())
    {
        const auto& clock = data_timestamps.samplingclock();
        if (!clock.has_starttime())
            return std::nullopt;
        const SourceTimestamp first = makeSourceTimestamp(clock.starttime());
        SourceTimestamp       last = first;
        const auto            count = static_cast<uint64_t>(clock.count());
        const auto            period_nanos = clock.periodnanos();
        if (count > 1 && period_nanos > 0)
        {
            const auto steps = count - 1;
            const auto offset_nanos = static_cast<unsigned __int128>(steps) *
                                      static_cast<unsigned __int128>(period_nanos);
            const auto add_secs = static_cast<uint64_t>(offset_nanos / 1'000'000'000ULL);
            const auto add_nanos = static_cast<uint64_t>(offset_nanos % 1'000'000'000ULL);
            last.epoch_seconds += add_secs;
            last.nanoseconds += add_nanos;
            if (last.nanoseconds >= 1'000'000'000ULL)
            {
                last.epoch_seconds += 1;
                last.nanoseconds -= 1'000'000'000ULL;
            }
        }
        return std::make_pair(first, last);
    }
    return std::nullopt;
}

} // namespace

QueryResult MLDPQueryClient::execute(std::string_view              table_name,
                                     const std::vector<Predicate>& pushable_predicates,
                                     const std::set<std::string>&  projection_hint,
                                     const ExecutionContext& context,
                                     std::string_view        page_token)
{
    if (table_name != kTimeSeriesTable && table_name != kTimeSeriesWideTable && table_name != kPvStatsTable)
        throw std::invalid_argument("MLDPQueryClient: unknown virtual table '" + std::string(table_name) +
                                    "'; supported tables: mldp.time_series, mldp.time_series_table, mldp.pv_stats");

    const auto pvs = requestedPvs(pushable_predicates);
    if (table_name == kPvStatsTable)
    {
        if (context.cancellation) context.cancellation->throwIfCancelled();
        std::size_t offset = 0;
        if (!page_token.empty())
        {
            try
            {
                const auto token = std::string(page_token);
                if (!token.starts_with("pv-stats:"))
                    throw std::invalid_argument("invalid");
                offset = static_cast<std::size_t>(std::stoull(token.substr(9)));
            }
            catch (const std::exception&)
            {
                throw std::invalid_argument("MLDP pv_stats continuation token is invalid");
            }
        }
        if (offset > pvs.size())
            throw std::invalid_argument("MLDP pv_stats continuation token is out of range");

        const auto                              page_size = context.join_batch_size == 0 ? pvs.size() : context.join_batch_size;
        const auto                              end = std::min(pvs.size(), offset + page_size);
        dp::service::query::QueryPvStatsRequest request;
        for (std::size_t index = offset; index < end; ++index)
            request.mutable_pvnamelist()->add_pvnames(pvs[index]);

        auto                                     handle = pool_->acquire();
        auto                                     rpc_context = std::make_shared<grpc::ClientContext>();
        dp::service::query::QueryPvStatsResponse response;
        auto cancellation_registration = context.cancellation
            ? context.cancellation->onCancel([rpc_context] { rpc_context->TryCancel(); })
            : QueryCancellation::Registration{};
        const auto                               status = handle->query_stub->queryPvStats(rpc_context.get(), request, &response);
        if (context.cancellation && context.cancellation->cancelled()) throw QueryCancelled{};
        if (!status.ok())
            throw std::runtime_error("MLDP queryPvStats failed: " + status.error_message());
        if (!response.has_statsresult())
            throw std::runtime_error("MLDP queryPvStats failed: " + response.exceptionalresult().message());

        arrow::StringBuilder    pv_builder;
        arrow::TimestampBuilder first_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::TimestampBuilder last_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::Int64Builder     buckets_builder;
        for (const auto& stat : response.statsresult().pvstats())
        {
            if (context.cancellation) context.cancellation->throwIfCancelled();
            if (!pv_builder.Append(stat.pvname()).ok() || !first_builder.Append(timestampToNanoseconds(stat.firstdatatimestamp())).ok() ||
                !last_builder.Append(timestampToNanoseconds(stat.lastdatatimestamp())).ok() || !buckets_builder.Append(stat.numbuckets()).ok())
                throw std::runtime_error("Failed to build Arrow pv_stats batch");
        }
        std::shared_ptr<arrow::Array> pv;
        std::shared_ptr<arrow::Array> first;
        std::shared_ptr<arrow::Array> last;
        std::shared_ptr<arrow::Array> buckets;
        if (!pv_builder.Finish(&pv).ok() || !first_builder.Finish(&first).ok() ||
            !last_builder.Finish(&last).ok() || !buckets_builder.Finish(&buckets).ok())
            throw std::runtime_error("Failed to finish Arrow pv_stats batch");
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()),
                                                                 arrow::field("first_timestamp", first->type()),
                                                                 arrow::field("last_timestamp", last->type()),
                                                                 arrow::field("num_buckets", arrow::int64())}),
                                                  pv->length(), {pv, first, last, buckets}),
                .next_page_token = end < pvs.size() ? "pv-stats:" + std::to_string(end) : ""};
    }

    std::size_t ts_offset = 0;
    if (!page_token.empty())
    {
        try
        {
            const auto token = std::string(page_token);
            if (!token.starts_with("ts:"))
                throw std::invalid_argument("invalid");
            ts_offset = static_cast<std::size_t>(std::stoull(token.substr(3)));
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument("MLDP time_series continuation token is invalid");
        }
    }

    const auto [begin, end] = requestedTimeRange(pushable_predicates);
    dp::service::query::QueryTableRequest request;
    request.set_format(dp::service::query::QueryTableRequest::TABLE_FORMAT_COLUMN);
    setTimestamp(request.mutable_begintime(), begin);
    setTimestamp(request.mutable_endtime(), end);
    for (const auto& pv_name : pvs)
        request.mutable_pvnamelist()->add_pvnames(pv_name);

    auto                                   handle = pool_->acquire();
    auto                                   rpc_context = std::make_shared<grpc::ClientContext>();
    dp::service::query::QueryTableResponse response;
    auto cancellation_registration = context.cancellation
        ? context.cancellation->onCancel([rpc_context] { rpc_context->TryCancel(); })
        : QueryCancellation::Registration{};
    if (context.cancellation) context.cancellation->throwIfCancelled();
    const auto                             status = handle->query_stub->queryTable(rpc_context.get(), request, &response);
    if (context.cancellation && context.cancellation->cancelled()) throw QueryCancelled{};
    if (!status.ok())
        throw std::runtime_error("MLDP queryTable failed: " + status.error_message());
    if (!response.has_tableresult())
        throw std::runtime_error("MLDP queryTable failed: " + response.exceptionalresult().message());

    const auto& col_table = response.tableresult().columntable();

    std::unordered_map<std::string, const dp::service::common::DataColumn*> returned;
    for (const auto& column : col_table.datacolumns())
    {
        if (!returned.emplace(column.name(), &column).second)
            throw std::runtime_error("MLDP queryTable returned duplicate PV column '" + column.name() + "'");
    }

    std::vector<const dp::service::common::DataColumn*> columns;
    columns.reserve(pvs.size());
    for (const auto& requested_pv : pvs)
    {
        const auto found = returned.find(requested_pv);
        if (found == returned.end())
            continue;
        if (matchesColumnPredicates(*found->second, pushable_predicates))
            columns.push_back(found->second);
    }

    std::vector<int64_t> timestamps_ns;
    const auto&          dt = col_table.datatimestamps();
    if (dt.has_timestamplist())
    {
        for (const auto& ts : dt.timestamplist().timestamps())
            timestamps_ns.push_back(timestampToNanoseconds(ts));
    }
    else if (dt.has_samplingclock())
    {
        const auto&   clock = dt.samplingclock();
        const int64_t start_ns = static_cast<int64_t>(clock.starttime().epochseconds()) * 1'000'000'000LL +
                                 static_cast<int64_t>(clock.starttime().nanoseconds());
        for (uint64_t i = 0; i < static_cast<uint64_t>(clock.count()); ++i)
            timestamps_ns.push_back(start_ns + static_cast<int64_t>(i) * static_cast<int64_t>(clock.periodnanos()));
    }

    if (table_name == kTimeSeriesWideTable)
    {
        if (!page_token.empty())
            throw std::invalid_argument("MLDP time_series_table does not support continuation tokens");
        if (columns.empty())
            return {.batch = nullptr, .next_page_token = ""};

        auto* pool = context.pool != nullptr ? context.pool : arrow::default_memory_pool();
        arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
        for (const auto timestamp : timestamps_ns)
            if (!time_builder.Append(timestamp).ok())
                throw std::runtime_error("Failed to append Arrow time-series table timestamp");

        std::shared_ptr<arrow::Array> time;
        if (!time_builder.Finish(&time).ok())
            throw std::runtime_error("Failed to finish Arrow time-series table timestamp column");

        std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("time", time->type())};
        std::vector<std::shared_ptr<arrow::Array>> arrays = {time};
        for (const auto* column : columns)
        {
            if (column->datavalues_size() > static_cast<int>(timestamps_ns.size()))
                throw std::runtime_error("MLDP queryTable PV column '" + column->name() + "' has more values than timestamps");

            std::shared_ptr<arrow::DataType> type = arrow::null();
            for (const auto& value : column->datavalues())
            {
                if (value.value_case() == dp::service::common::DataValue::VALUE_NOT_SET)
                    continue;
                const auto candidate = dataValueArrowType(value);
                if (type->id() == arrow::Type::NA)
                    type = candidate;
                else if (!type->Equals(candidate))
                    throw std::runtime_error("MLDP queryTable PV column '" + column->name() + "' contains mixed data types");
            }

            std::unique_ptr<arrow::ArrayBuilder> builder;
            const auto builder_status = arrow::MakeBuilder(pool, type, &builder);
            if (!builder_status.ok())
                throw std::runtime_error("Failed to create Arrow builder for MLDP PV column '" + column->name() + "': " + builder_status.ToString());
            for (const auto& value : column->datavalues())
                appendNativeValue(*builder, value);
            for (int index = column->datavalues_size(); index < static_cast<int>(timestamps_ns.size()); ++index)
                if (!builder->AppendNull().ok())
                    throw std::runtime_error("Failed to append trailing null for MLDP PV column '" + column->name() + "'");

            std::shared_ptr<arrow::Array> values;
            if (!builder->Finish(&values).ok())
                throw std::runtime_error("Failed to finish Arrow MLDP PV column '" + column->name() + "'");
            fields.push_back(arrow::field(column->name(), values->type(), true, arrowFieldMetadata(column->metadata())));
            arrays.push_back(std::move(values));
        }
        return {.batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), time->length(), std::move(arrays)), .next_page_token = ""};
    }

    struct Row
    {
        std::string                         pv_name;
        int64_t                             time_ns;
        dp::service::common::DataValue      value;
        dp::service::common::ColumnMetadata metadata;
    };

    std::vector<Row> all_rows;
    for (const auto* column : columns)
    {
        for (int i = 0; i < column->datavalues_size(); ++i)
        {
            const int64_t ts = (i < static_cast<int>(timestamps_ns.size())) ? timestamps_ns[i] : 0;
            all_rows.push_back({column->name(), ts, column->datavalues(i), column->metadata()});
        }
    }
    std::stable_sort(all_rows.begin(), all_rows.end(),
                     [](const Row& a, const Row& b)
                     {
                         return a.time_ns < b.time_ns;
                     });

    std::vector<dp::service::common::ColumnMetadata> all_metadata;
    all_metadata.reserve(all_rows.size());
    for (const auto& row : all_rows)
    {
        all_metadata.push_back(row.metadata);
    }

    if (ts_offset > all_rows.size())
        ts_offset = all_rows.size();

    const auto page_sz = context.join_batch_size == 0 ? all_rows.size() : context.join_batch_size;
    const auto page_end = std::min(all_rows.size(), ts_offset + page_sz);

    auto*                      pool = context.pool != nullptr ? context.pool : arrow::default_memory_pool();
    arrow::StringBuilder       pv_builder;
    arrow::TimestampBuilder    time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
    DataValueBuilder           value_builder(pool);
    arrow::StringBuilder       type_builder(pool);
    auto attribute_keys = attributeKeys(all_metadata);
    auto provenance_keys = provenanceKeys(all_metadata);
    addRequestedDynamicMetadataKeys(attribute_keys, projection_hint, "attributes.");
    addRequestedDynamicMetadataKeys(provenance_keys, projection_hint, "provenance.");
    TimeSeriesMetadataBuilders metadata(attribute_keys, provenance_keys);
    for (std::size_t i = ts_offset; i < page_end; ++i)
    {
        if (!pv_builder.Append(all_rows[i].pv_name).ok() || !time_builder.Append(all_rows[i].time_ns).ok())
            throw std::runtime_error("Failed to build Arrow time-series batch");
        value_builder.append(all_rows[i].value);
        if (!type_builder.Append(dataValueKind(all_rows[i].value)).ok())
            throw std::runtime_error("Failed to build Arrow time-series column_type");
        metadata.append(all_rows[i].metadata);
    }
    std::shared_ptr<arrow::Array> pv;
    std::shared_ptr<arrow::Array> time;
    if (!pv_builder.Finish(&pv).ok() || !time_builder.Finish(&time).ok())
        throw std::runtime_error("Failed to finish Arrow time-series batch");
    const auto                                 value = value_builder.finish();
    std::shared_ptr<arrow::Array> type;
    if (!type_builder.Finish(&type).ok())
        throw std::runtime_error("Failed to finish Arrow time-series column_type");
    std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("pv", arrow::utf8()), arrow::field("time", time->type()), arrow::field("value", value->type()), arrow::field("column_type", type->type())};
    std::vector<std::shared_ptr<arrow::Array>> arrays = {pv, time, value, type};
    metadata.finish(fields, arrays);
    return {.batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), pv->length(), std::move(arrays)),
            .next_page_token = page_end < all_rows.size() ? "ts:" + std::to_string(page_end) : ""};
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MLDPQueryClient::MLDPQueryClient(const util::pool::MLDPGrpcPoolConfig& poolConfig,
                                 std::shared_ptr<metrics::Metrics>     metrics)
    : logger_(makeQueryClientLogger())
    , pool_(MLDPGrpcQueryPool::create(poolConfig, std::move(metrics)))
{
}

MLDPQueryClient::MLDPQueryClient(const util::pool::MLDPGrpcQueryPoolConfig& poolConfig,
                                 std::shared_ptr<metrics::Metrics>          metrics)
    : logger_(makeQueryClientLogger())
    , pool_(MLDPGrpcQueryPool::create(poolConfig, std::move(metrics)))
{
}

MLDPQueryClient::MLDPQueryClient(const config::Config&             cfg,
                                 std::shared_ptr<metrics::Metrics> m)
{
    logger_ = makeQueryClientLogger();
    if (cfg.hasChild(util::pool::IngestionUrlKey))
    {
        pool_ = MLDPGrpcQueryPool::create(util::pool::MLDPGrpcPoolConfig(cfg), m);
    }
    else
    {
        pool_ = MLDPGrpcQueryPool::create(util::pool::MLDPGrpcQueryPoolConfig(cfg), m);
    }
}

// ---------------------------------------------------------------------------
// querySourcesInfo
// ---------------------------------------------------------------------------

std::vector<IDataBus::SourceInfo>
MLDPQueryClient::querySourcesInfo(const std::set<std::string>& source_names)
{
    std::vector<IDataBus::SourceInfo> infos;
    if (source_names.empty())
        return infos;

    try
    {
        auto  handle = pool_->acquire();
        auto* query_stub = handle->query_stub.get();
        if (!query_stub)
        {
            handle->query_stub = handle->makeQueryStub();
            query_stub = handle->query_stub.get();
        }
        if (!query_stub)
        {
            errorf(*logger_, "Failed to create query stub for source metadata request");
            return infos;
        }

        dp::service::query::QueryPvStatsRequest request;
        auto*                                   pv_name_list = request.mutable_pvnamelist();
        pv_name_list->mutable_pvnames()->Reserve(static_cast<int>(source_names.size()));
        for (const auto& source : source_names)
        {
            if (!source.empty())
                pv_name_list->add_pvnames(source);
        }
        if (pv_name_list->pvnames().empty())
            return infos;

        grpc::ClientContext                      context;
        dp::service::query::QueryPvStatsResponse response;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        const auto status = query_stub->queryPvStats(&context, request, &response);

        if (!status.ok())
        {
            const bool metadata_rpc_missing =
                status.error_code() == grpc::StatusCode::UNIMPLEMENTED ||
                status.error_message().find("Method not found") != std::string::npos;
            if (!metadata_rpc_missing)
            {
                errorf(*logger_, "queryPvStats RPC failed: {}", status.error_message());
                return infos;
            }

            warnf(*logger_,
                  "queryPvStats unavailable ({}). Falling back to queryData-derived timestamps.",
                  status.error_message());

            dp::service::query::QueryDataRequest data_request;
            auto*                                spec = data_request.mutable_queryspec();
            for (const auto& source : source_names)
            {
                if (!source.empty())
                    spec->add_pvnames(source);
            }
            if (spec->pvnames().empty())
                return infos;

            auto* begin_ts = spec->mutable_begintime();
            begin_ts->set_epochseconds(0);
            auto* end_ts = spec->mutable_endtime();
            end_ts->set_epochseconds(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()) +
                1);

            grpc::ClientContext                   data_context;
            dp::service::query::QueryDataResponse data_response;
            data_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
            const auto data_status = query_stub->queryData(&data_context, data_request, &data_response);
            if (!data_status.ok())
            {
                errorf(*logger_, "queryData fallback RPC failed: {}", data_status.error_message());
                return infos;
            }
            if (!data_response.has_querydata() || data_response.has_exceptionalresult())
            {
                return infos;
            }

            std::unordered_map<std::string, IDataBus::SourceInfo> merged_infos;
            for (const auto& bucket : data_response.querydata().databuckets())
            {
                const auto& pvname = bucket.pvname();
                if (pvname.empty() || !source_names.contains(pvname))
                    continue;

                auto& info = merged_infos[pvname];
                if (info.source_name.empty())
                {
                    info.source_name = pvname;
                    info.num_buckets = 0;
                }
                if (info.num_buckets.has_value())
                {
                    info.num_buckets = info.num_buckets.value() + 1;
                }
                if (!bucket.has_datatimestamps())
                    continue;

                const auto range = extractTimestampRange(bucket.datatimestamps());
                if (!range.has_value())
                    continue;

                const auto& [bucket_first, bucket_last] = range.value();
                if (!info.first_timestamp.has_value() || isBefore(bucket_first, info.first_timestamp.value()))
                {
                    info.first_timestamp = bucket_first;
                }
                if (!info.last_timestamp.has_value() || isBefore(info.last_timestamp.value(), bucket_last))
                {
                    info.last_timestamp = bucket_last;
                }
                const auto& data_timestamps = bucket.datatimestamps();
                if (data_timestamps.has_samplingclock())
                {
                    const auto& clock = data_timestamps.samplingclock();
                    info.last_bucket_sample_period = clock.periodnanos();
                    info.last_bucket_sample_count = clock.count();
                    info.last_bucket_data_timestamps_type = "SAMPLING_CLOCK";
                }
                else if (data_timestamps.has_timestamplist())
                {
                    info.last_bucket_sample_count =
                        static_cast<uint32_t>(data_timestamps.timestamplist().timestamps_size());
                    info.last_bucket_data_timestamps_type = "TIMESTAMP_LIST";
                }
            }

            infos.reserve(merged_infos.size());
            for (auto& [_, info] : merged_infos)
            {
                infos.push_back(std::move(info));
            }
            return infos;
        }

        if (response.has_exceptionalresult())
        {
            errorf(*logger_, "queryPvStats returned exceptional result: {}",
                   response.exceptionalresult().message());
            return infos;
        }
        if (!response.has_statsresult())
            return infos;

        const auto& pv_infos = response.statsresult().pvstats();
        infos.reserve(static_cast<std::size_t>(pv_infos.size()));
        for (const auto& pv_info : pv_infos)
        {
            IDataBus::SourceInfo info;
            info.source_name = pv_info.pvname();
            if (pv_info.has_firstdatatimestamp())
                info.first_timestamp = makeSourceTimestamp(pv_info.firstdatatimestamp());
            if (pv_info.has_lastdatatimestamp())
                info.last_timestamp = makeSourceTimestamp(pv_info.lastdatatimestamp());
            if (!pv_info.lastproviderid().empty())
                info.last_provider_id = pv_info.lastproviderid();
            if (!pv_info.lastprovidername().empty())
                info.last_provider_name = pv_info.lastprovidername();
            if (!pv_info.lastbucketid().empty())
                info.last_bucket_id = pv_info.lastbucketid();
            if (!pv_info.lastbucketdatatype().empty())
                info.last_bucket_data_type = pv_info.lastbucketdatatype();
            if (!pv_info.lastbucketdatatimestampstype().empty())
                info.last_bucket_data_timestamps_type = pv_info.lastbucketdatatimestampstype();
            if (pv_info.lastbucketsampleperiod() > 0)
                info.last_bucket_sample_period = pv_info.lastbucketsampleperiod();
            if (pv_info.lastbucketsamplecount() > 0)
                info.last_bucket_sample_count = pv_info.lastbucketsamplecount();
            info.num_buckets = pv_info.numbuckets();
            infos.push_back(std::move(info));
        }
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "querySourcesInfo failed: {}", ex.what());
    }
    return infos;
}

// ---------------------------------------------------------------------------
// querySourcesData
// ---------------------------------------------------------------------------

std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>>
MLDPQueryClient::querySourcesData(const std::set<std::string>&   source_names,
                                  const QuerySourcesDataOptions& options)
{
    if (source_names.empty())
    {
        return std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>{};
    }
    if (options.timeout <= std::chrono::milliseconds::zero())
    {
        warnf(*logger_, "querySourcesData timeout must be > 0");
        return std::nullopt;
    }

    try
    {
        auto  handle = pool_->acquire();
        auto* query_stub = handle->query_stub.get();
        if (!query_stub)
        {
            handle->query_stub = handle->makeQueryStub();
            query_stub = handle->query_stub.get();
        }
        if (!query_stub)
        {
            errorf(*logger_, "Failed to create query stub for source data request");
            return std::nullopt;
        }

        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            dp::service::query::QueryDataRequest request;
            auto*                                spec = request.mutable_queryspec();
            for (const auto& source : source_names)
            {
                if (!source.empty())
                    spec->add_pvnames(source);
            }
            if (spec->pvnames().empty())
            {
                return std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>{};
            }

            const auto now = std::chrono::system_clock::now();
            const auto begin = now - options.lookback_window;
            const auto end = now + options.forward_window;
            auto*      begin_ts = spec->mutable_begintime();
            begin_ts->set_epochseconds(
                std::chrono::duration_cast<std::chrono::seconds>(begin.time_since_epoch()).count());
            auto* end_ts = spec->mutable_endtime();
            end_ts->set_epochseconds(
                std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()).count());

            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + options.rpc_deadline);

            dp::service::query::QueryDataResponse response;
            const auto                            status = query_stub->queryData(&context, request, &response);
            if (status.ok() && response.has_querydata() && !response.has_exceptionalresult())
            {
                std::unordered_map<std::string, std::vector<dp::service::common::DataValues>> collected;
                for (const auto& bucket : response.querydata().databuckets())
                {
                    const auto& pvname = bucket.pvname();
                    if (pvname.empty() || !source_names.contains(pvname) || !bucket.has_datavalues())
                        continue;
                    collected[pvname].push_back(bucket.datavalues());
                }
                if (collected.size() == source_names.size())
                    return collected;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "querySourcesData failed: {}", ex.what());
        return std::nullopt;
    }
    return std::nullopt;
}
