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
#include <query/impl/mldp/ColumnPredicateFilter.h>
#include <query/impl/mldp/DataValueBuilder.h>
#include <query/impl/mldp/MldpBidiRecordBatchStream.h>
#include <query/impl/mldp/MldpQueryBucketsPagedStream.h>
#include <query/impl/mldp/MldpQuerySamplesPagedStream.h>
#include <query/impl/mldp/MldpTimestampUtils.h>
#include <query/impl/mldp/ParallelSeriesRecordBatchStream.h>

#include <pool/MLDPGrpcQueryPoolConfig.h>

#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/QueryProgress.h>
#include <query/SpillBackedStream.h>

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
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

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

std::size_t MLDPQueryClient::maxConcurrentStreams() const noexcept
{
    return pool_ ? pool_->maxSize() : 1;
}

std::vector<ColumnSchema> MLDPQueryClient::tableSchema(std::string_view table_name) const
{
    const std::set<PredicateOp> kPvOps = {PredicateOp::EQ, PredicateOp::IN, PredicateOp::PREFIX, PredicateOp::CONTAINS, PredicateOp::LIKE};
    if (table_name == "mldp.time_series" || table_name == "mldp.time_series_table")
    {
        const bool                wide_table = table_name == "mldp.time_series_table";
        std::vector<ColumnSchema> schema = {
            {"pv", ColumnType::STRING, false, !wide_table, kPvOps, kPvOps, "Source name; = / IN select explicit PVs, PREFIX / CONTAINS / LIKE push a PV-name regex; omit to scan every PV"},
            {"time", ColumnType::TIMESTAMP, false, true, {PredicateOp::GTE, PredicateOp::LTE}, {}, "Sample timestamp"},
            {"value", ColumnType::NATIVE_VALUE, false, !wide_table, {}, {PredicateOp::EQ, PredicateOp::NEQ, PredicateOp::LT, PredicateOp::LTE, PredicateOp::GT, PredicateOp::GTE, PredicateOp::IN, PredicateOp::BETWEEN}, "Native sample value"},
            {"column_type", ColumnType::STRING, false, !wide_table, {PredicateOp::EQ, PredicateOp::IN}, {PredicateOp::EQ, PredicateOp::IN}, "Native MLDP data-value type"},
            {"tags", ColumnType::STRING, false, !wide_table, {}, {}, "Bucket column-metadata tag collection; filter with tag = or tag IN locally"},
            {"attributes", ColumnType::STRING, false, !wide_table, {}, {}, "Bucket column-metadata dynamic attribute map; select/filter attributes.<key> locally"},
            {"provenance", ColumnType::STRING, false, !wide_table, {}, {}, "Bucket column-metadata dynamic provenance map; select/filter provenance.<key> locally"},
            {"tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {PredicateOp::EQ, PredicateOp::IN}, "Tag membership predicate shorthand for tags"},
            {"pv_tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "PV metadata tag selector (backend PvSelector.metadataQuery); pv_attributes.<key> selects by PV metadata attribute"},
            {"config_name", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Restrict samples to activation intervals of the named configuration(s)"},
            {"config_activation_id", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Restrict samples to the given client activation id(s)"},
            {"config_category", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Restrict samples to activations of configurations in the category(ies)"},
            {"config_tag", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Restrict samples to activations of configurations carrying the tag(s)"},
            {"timeout", ColumnType::DURATION_SECONDS, false, false, {PredicateOp::EQ}, {}, "Query timeout"},
            {"rpc_deadline", ColumnType::DURATION_SECONDS, false, false, {PredicateOp::EQ}, {}, "RPC deadline"}};
        {
            // Sample-status filtering runs on the sample-oriented backend query.
            schema.push_back({"status_domain", ColumnType::STRING, false, false, {PredicateOp::EQ}, {}, "Sample status domain; enables sample status filtering"});
            schema.push_back({"status_layer", ColumnType::STRING, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Sample status layer(s); omit for all layers in the domain"});
            schema.push_back({"status_code", ColumnType::INT, false, false, {PredicateOp::EQ, PredicateOp::IN}, {}, "Sample status code(s); omit for any code"});
            schema.push_back({"status_mode", ColumnType::STRING, false, false, {PredicateOp::EQ}, {}, "'include' (default) keeps only matching samples, 'exclude' drops them"});
        }
        schema.emplace_back("window", ColumnType::TIMESTAMP, false, false, std::set<PredicateOp>{PredicateOp::IN}, std::set<PredicateOp>{},
                            "Time-series interval input; accepts window IN (start, end) or window IN (SELECT start_time, end_time ...)");
        return schema;
    }
    if (table_name == "mldp.pv_stats")
    {
        return {{"pv", ColumnType::STRING, false, true, kPvOps, kPvOps, "Source name; = / IN select explicit PVs, PREFIX / CONTAINS / LIKE push a PV-name regex; omit to scan every PV"},
                {"start_time", ColumnType::TIMESTAMP, false, true, {}, {}, "First observed sample time"},
                {"end_time", ColumnType::TIMESTAMP, false, true, {}, {}, "Last observed sample time"},
                {"num_buckets", ColumnType::INT, false, true, {}, {}, "Number of buckets"}};
    }
    throw std::invalid_argument("MLDPQueryClient: unknown virtual table: " + std::string(table_name));
}

namespace {

constexpr std::string_view kTimeSeriesTable = "mldp.time_series";
constexpr std::string_view kTimeSeriesWideTable = "mldp.time_series_table";
constexpr std::string_view kPvStatsTable = "mldp.pv_stats";

bool isPvListPredicate(const Predicate& predicate)
{
    return predicate.column == "pv" && (predicate.op == PredicateOp::EQ || predicate.op == PredicateOp::IN);
}

bool isPvPatternPredicate(const Predicate& predicate)
{
    return predicate.column == "pv" &&
           (predicate.op == PredicateOp::PREFIX || predicate.op == PredicateOp::CONTAINS || predicate.op == PredicateOp::LIKE);
}

std::vector<std::string> predicateStrings(const Predicate& predicate)
{
    std::vector<std::string> values;
    for (const auto& value : predicate.values)
    {
        if (!std::holds_alternative<std::string>(value))
            throw std::invalid_argument("MLDP query predicate " + predicate.column + " requires string values");
        values.push_back(std::get<std::string>(value));
    }
    return values;
}

std::string escapeRegex(std::string_view text)
{
    std::string escaped;
    for (const auto character : text)
    {
        if (std::string_view("\\^$.|?*+()[]{}").find(character) != std::string_view::npos)
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

// Backend regex for a PV-name pattern predicate. LIKE is case-insensitive
// locally, so the backend pattern is too; rows are re-verified locally.
std::string pvNameRegex(const Predicate& predicate)
{
    const auto values = predicateStrings(predicate);
    if (values.size() != 1)
        throw std::invalid_argument("MLDP pv pattern predicate requires one string value");
    const auto& pattern = values.front();
    if (predicate.op == PredicateOp::PREFIX)
        return "^" + escapeRegex(pattern) + ".*$";
    if (predicate.op == PredicateOp::CONTAINS)
        return "^.*" + escapeRegex(pattern) + ".*$";
    std::string regex = "(?i)^";
    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
        const auto character = pattern[index];
        if (character == '\\' && index + 1 < pattern.size())
            regex += escapeRegex(std::string_view(&pattern[++index], 1));
        else if (character == '%' || character == '*')
            regex += ".*";
        else if (character == '_')
            regex += ".";
        else
            regex += escapeRegex(std::string_view(&character, 1));
    }
    return regex + "$";
}

// Explicit PV names from pv = / pv IN; empty when the query selects PVs by
// pattern, metadata, or not at all.
std::vector<std::string> requestedPvs(const std::vector<Predicate>& predicates)
{
    std::vector<std::string> names;
    std::set<std::string>    seen;
    for (const auto& predicate : predicates)
    {
        if (!isPvListPredicate(predicate))
            continue;
        for (auto& name : predicateStrings(predicate))
            if (seen.insert(name).second)
                names.push_back(std::move(name));
    }
    return names;
}

// Removes the explicit PV-list predicates so a shard can substitute its own.
std::vector<Predicate> withoutPvListPredicates(std::vector<Predicate> predicates)
{
    predicates.erase(std::remove_if(predicates.begin(), predicates.end(), isPvListPredicate), predicates.end());
    return predicates;
}

std::string pvNamePatternFor(const std::vector<Predicate>& predicates)
{
    // Only one regex can be pushed; further pattern predicates stay local.
    for (const auto& predicate : predicates)
        if (isPvPatternPredicate(predicate))
            return pvNameRegex(predicate);
    // The protocol's explicit all-PV form.
    return ".*";
}

void applyPvSelector(dp::service::query::PvSelector& selector, const std::vector<Predicate>& predicates)
{
    using MetadataCriterion = dp::service::query::PvSelector::MetadataQuery::Criterion;
    const auto pvs = requestedPvs(predicates);
    std::vector<MetadataCriterion> metadata_criteria;
    for (const auto& predicate : predicates)
    {
        const bool attribute = predicate.column.rfind("pv_attributes.", 0) == 0;
        if (predicate.column != "pv_tag" && !attribute)
            continue;
        if (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)
            throw std::invalid_argument("MLDP " + predicate.column + " supports only = or IN");
        auto& criterion = metadata_criteria.emplace_back();
        if (attribute)
        {
            auto* target = criterion.mutable_attributescriterion();
            target->set_key(predicate.column.substr(std::string("pv_attributes.").size()));
            for (const auto& value : predicateStrings(predicate))
                target->add_values(value);
        }
        else
            for (const auto& value : predicateStrings(predicate))
                criterion.mutable_tagscriterion()->add_values(value);
    }
    if (!metadata_criteria.empty())
    {
        // PV-name restrictions join the metadata query as a name criterion;
        // LIKE has no backend form there and is verified locally only.
        MetadataCriterion name_criterion;
        auto*             names = name_criterion.mutable_pvnamecriterion();
        for (const auto& pv : pvs)
            names->add_exact(pv);
        for (const auto& predicate : predicates)
        {
            if (predicate.column != "pv" || (predicate.op != PredicateOp::PREFIX && predicate.op != PredicateOp::CONTAINS))
                continue;
            for (const auto& value : predicateStrings(predicate))
                predicate.op == PredicateOp::PREFIX ? names->add_prefix(value) : names->add_contains(value);
        }
        auto* query = selector.mutable_metadataquery();
        for (auto& criterion : metadata_criteria)
            *query->add_criteria() = std::move(criterion);
        if (names->exact_size() + names->prefix_size() + names->contains_size() > 0)
            *query->add_criteria() = std::move(name_criterion);
        return;
    }
    if (!pvs.empty())
    {
        for (const auto& pv : pvs)
            selector.mutable_pvnamelist()->add_pvnames(pv);
        return;
    }
    selector.mutable_pvnamepattern()->set_pattern(pvNamePatternFor(predicates));
}

void applyConfigurationSelector(dp::service::query::QuerySpec& spec, const std::vector<Predicate>& predicates)
{
    using Criterion = dp::service::query::ConfigurationSelector::Criterion;
    std::vector<Criterion> criteria;
    for (const auto& predicate : predicates)
    {
        if (predicate.column.rfind("config_", 0) != 0)
            continue;
        auto& criterion = criteria.emplace_back();
        const auto values = predicateStrings(predicate);
        for (const auto& value : values)
        {
            if (predicate.column == "config_name")
                criterion.mutable_configurationnamecriterion()->add_values(value);
            else if (predicate.column == "config_activation_id")
                criterion.mutable_clientactivationidcriterion()->add_values(value);
            else if (predicate.column == "config_category")
                criterion.mutable_categorycriterion()->add_values(value);
            else if (predicate.column == "config_tag")
                criterion.mutable_tagscriterion()->add_values(value);
            else
                throw std::invalid_argument("Unsupported MLDP configuration selector column: " + predicate.column);
        }
    }
    // The backend rejects an empty selector; omit it when nothing was pushed.
    if (criteria.empty())
        return;
    auto* selector = spec.mutable_configurationselector();
    for (auto& criterion : criteria)
        *selector->add_criteria() = std::move(criterion);
}

void applySampleStatusSelector(dp::service::query::QuerySpec& spec, const std::vector<Predicate>& predicates)
{
    using Selector = dp::service::query::SampleStatusSelector;
    std::optional<std::string> domain;
    Selector                   selector;
    selector.set_mode(Selector::MODE_INCLUDE_MATCHING);
    bool has_status_predicate = false;
    for (const auto& predicate : predicates)
    {
        if (predicate.column.rfind("status_", 0) != 0)
            continue;
        has_status_predicate = true;
        if (predicate.column == "status_code")
        {
            for (const auto& value : predicate.values)
            {
                if (!std::holds_alternative<int64_t>(value))
                    throw std::invalid_argument("MLDP status_code requires integer values");
                selector.add_statuscodes(static_cast<int32_t>(std::get<int64_t>(value)));
            }
            continue;
        }
        const auto values = predicateStrings(predicate);
        if (predicate.column == "status_domain")
            domain = values.at(0);
        else if (predicate.column == "status_layer")
            for (const auto& value : values)
                selector.add_layers(value);
        else if (predicate.column == "status_mode")
        {
            if (values.at(0) == "include")
                selector.set_mode(Selector::MODE_INCLUDE_MATCHING);
            else if (values.at(0) == "exclude")
                selector.set_mode(Selector::MODE_EXCLUDE_MATCHING);
            else
                throw std::invalid_argument("MLDP status_mode must be 'include' or 'exclude'");
        }
        else
            throw std::invalid_argument("Unsupported MLDP sample status column: " + predicate.column);
    }
    if (!has_status_predicate)
        return;
    if (!domain)
        throw std::invalid_argument("MLDP sample status filtering requires status_domain =");
    selector.set_domain(*domain);
    *spec.mutable_samplestatusselector() = std::move(selector);
}

// configurationSelector and sampleStatusSelector are exact only on the
// sample-oriented query: bucket queries return every bucket overlapping a
// matching activation whole.
bool needsSampleQuery(const std::vector<Predicate>& predicates)
{
    return std::any_of(predicates.begin(), predicates.end(), [](const Predicate& predicate)
                       { return predicate.column.rfind("config_", 0) == 0 || predicate.column.rfind("status_", 0) == 0; });
}

/** Long-form mldp.time_series rows from querySamples pages.
 *
 *  Each page's columns become one DataBucket per PV holding only that PV's
 *  present samples, so the shared bucket decoder yields the usual columns. */
class SamplesLongRecordBatchStream final : public IRecordBatchStream
{
public:
    SamplesLongRecordBatchStream(MldpQuerySamplesPagedStream pages, std::vector<Predicate> predicates, std::set<std::string> projection_hint, arrow::MemoryPool* pool)
        : pages_(std::move(pages)), predicates_(std::move(predicates)), projection_hint_(std::move(projection_hint)), pool_(pool)
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        while (auto page = pages_.next())
        {
            ::google::protobuf::RepeatedPtrField<dp::service::common::DataBucket> buckets;
            const auto& timestamps = page->timestamplist().timestamps();
            for (const auto& column : page->datacolumns())
            {
                dp::service::common::DataBucket bucket;
                bucket.set_pvname(column.name());
                auto* bucket_times = bucket.mutable_datatimestamps()->mutable_timestamplist();
                auto* bucket_values = bucket.mutable_datavalues()->mutable_datacolumn();
                bucket_values->set_name(column.name());
                *bucket_values->mutable_metadata() = column.metadata();
                for (int index = 0; index < column.datavalues_size() && index < timestamps.size(); ++index)
                {
                    // Unset values are samples filtered out or absent at this timestamp.
                    if (column.datavalues(index).value_case() == dp::service::common::DataValue::VALUE_NOT_SET) continue;
                    *bucket_times->add_timestamps() = timestamps[index];
                    *bucket_values->add_datavalues() = column.datavalues(index);
                }
                if (bucket_values->datavalues_size() > 0) *buckets.Add() = std::move(bucket);
            }
            if (buckets.empty()) continue;
            auto batch = decodeDataBucketsToBatch(buckets, predicates_, projection_hint_, pool_);
            if (batch && batch->num_rows() > 0) return batch;
        }
        return nullptr;
    }

private:
    MldpQuerySamplesPagedStream pages_;
    std::vector<Predicate>      predicates_;
    std::set<std::string>       projection_hint_;
    arrow::MemoryPool*          pool_;
};

dp::service::query::QuerySpec buildQuerySpec(const std::vector<Predicate>& predicates,
                                             const std::pair<int64_t, int64_t>& time_range,
                                             const bool                     sample_oriented)
{
    dp::service::query::QuerySpec spec;
    setTimestamp(spec.mutable_timerange()->mutable_begintime(), time_range.first);
    setTimestamp(spec.mutable_timerange()->mutable_endtime(), time_range.second);
    applyPvSelector(*spec.mutable_pvselector(), predicates);
    applyConfigurationSelector(spec, predicates);
    // Bucket queries reject a status selector; callers route those scans to
    // querySamples (needsSampleQuery), so it is only set there.
    if (sample_oriented) applySampleStatusSelector(spec, predicates);
    return spec;
}

bool matchesColumnPredicates(const dp::service::common::DataColumn& column,
                             const std::vector<Predicate>&          predicates)
{
    return matchesPvNamePredicates(column.name(), predicates) &&
           matchesColumnMetadataPredicates(column.metadata(), dataValuesKind(column.datavalues()), predicates);
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
        if (!builder.AppendNull().ok())
            throw std::runtime_error("Failed to append null MLDP data value");
        return;
    }
    switch (value.value_case())
    {
    case dp::service::common::DataValue::kStringValue:
        if (!dynamic_cast<arrow::StringBuilder&>(builder).Append(value.stringvalue()).ok())
            throw std::runtime_error("Failed to append string");
        break;
    case dp::service::common::DataValue::kBooleanValue:
        if (!dynamic_cast<arrow::BooleanBuilder&>(builder).Append(value.booleanvalue()).ok())
            throw std::runtime_error("Failed to append bool");
        break;
    case dp::service::common::DataValue::kUintValue:
        if (!dynamic_cast<arrow::UInt32Builder&>(builder).Append(value.uintvalue()).ok())
            throw std::runtime_error("Failed to append uint32");
        break;
    case dp::service::common::DataValue::kUlongValue:
        if (!dynamic_cast<arrow::UInt64Builder&>(builder).Append(value.ulongvalue()).ok())
            throw std::runtime_error("Failed to append uint64");
        break;
    case dp::service::common::DataValue::kIntValue:
        if (!dynamic_cast<arrow::Int32Builder&>(builder).Append(value.intvalue()).ok())
            throw std::runtime_error("Failed to append int32");
        break;
    case dp::service::common::DataValue::kLongValue:
        if (!dynamic_cast<arrow::Int64Builder&>(builder).Append(value.longvalue()).ok())
            throw std::runtime_error("Failed to append int64");
        break;
    case dp::service::common::DataValue::kFloatValue:
        if (!dynamic_cast<arrow::FloatBuilder&>(builder).Append(value.floatvalue()).ok())
            throw std::runtime_error("Failed to append float");
        break;
    case dp::service::common::DataValue::kDoubleValue:
        if (!dynamic_cast<arrow::DoubleBuilder&>(builder).Append(value.doublevalue()).ok())
            throw std::runtime_error("Failed to append double");
        break;
    case dp::service::common::DataValue::kByteArrayValue:
        if (!dynamic_cast<arrow::BinaryBuilder&>(builder).Append(value.bytearrayvalue()).ok())
            throw std::runtime_error("Failed to append binary");
        break;
    case dp::service::common::DataValue::kTimestampValue:
        if (!dynamic_cast<arrow::TimestampBuilder&>(builder).Append(timestampToNanoseconds(value.timestampvalue())).ok())
            throw std::runtime_error("Failed to append timestamp");
        break;
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

IRecordBatchStreamUPtr MLDPQueryClient::executeStream(const std::string_view        table_name,
                                                      const std::vector<Predicate>& pushable_predicates,
                                                      const std::set<std::string>&  projection_hint,
                                                      const ExecutionContext&       context)
{
    if (table_name != kTimeSeriesTable && table_name != kTimeSeriesWideTable && table_name != kPvStatsTable)
        throw std::invalid_argument("MLDPQueryClient: unknown virtual table '" + std::string(table_name) +
                                    "'; supported tables: mldp.time_series, mldp.time_series_table, mldp.pv_stats");

    // -----------------------------------------------------------------------
    // mldp.time_series — native bidi stream, returned lazily so a LIMIT or an
    // interactive page stops pulling backend pages once it is satisfied
    // -----------------------------------------------------------------------
    if (table_name == kTimeSeriesTable)
    {
        const auto pvs = requestedPvs(pushable_predicates);
        IRecordBatchStreamUPtr raw_stream;
        if (context.series_per_shard != 0 && pvs.size() > context.series_per_shard)
        {
            raw_stream = std::make_unique<ParallelSeriesRecordBatchStream>(
                *this, std::string(table_name), pushable_predicates, projection_hint, context, pvs);
        }
        else
        {
            const bool sample_query = needsSampleQuery(pushable_predicates);
            auto       spec = buildQuerySpec(pushable_predicates, requestedTimeRange(pushable_predicates), sample_query);
            if (sample_query)
                return std::make_unique<SamplesLongRecordBatchStream>(MldpQuerySamplesPagedStream(pool_->acquire(), std::move(spec), context),
                                                                      pushable_predicates, projection_hint,
                                                                      context.pool != nullptr ? context.pool : arrow::default_memory_pool());
            raw_stream = std::make_unique<MldpQueryBucketsPagedStream>(pool_->acquire(), std::move(spec), pushable_predicates, projection_hint, context);
        }
        return raw_stream;
    }

    // -----------------------------------------------------------------------
    // mldp.pv_stats — parallel sharded queryPvStats, spill result
    // -----------------------------------------------------------------------
    if (table_name == kPvStatsTable)
    {
        if (context.cancellation)
            context.cancellation->throwIfCancelled();
        const auto pvs = requestedPvs(pushable_predicates);
        const auto request_count = pvs.size();
        const auto shard_size = context.series_per_shard == 0
                                    ? request_count
                                    : std::min<std::size_t>(request_count, static_cast<std::size_t>(context.series_per_shard));
        const auto capability_limit = std::max<std::size_t>(1, maxConcurrentStreams());
        const auto parallel_limit = context.max_parallel_requests == 0
                                        ? capability_limit
                                        : std::min<std::size_t>(capability_limit, context.max_parallel_requests);
        using PvStats = dp::service::query::QueryPvStatsResponse_StatsResult_PvStats;

        struct StatsShard
        {
            std::vector<PvStats> stats;
        };

        const auto                           shard_count = shard_size > 0 ? (request_count + shard_size - 1) / shard_size : 0;
        std::vector<std::future<StatsShard>> futures;
        futures.reserve(shard_count);
        if (context.progress)
        {
            context.progress->setActivity(std::string(kPvStatsTable), "parallel series-shard scan", "querying PV statistics");
            context.progress->setParallelShards(static_cast<uint64_t>(std::min(parallel_limit, shard_count)),
                                                static_cast<uint64_t>(std::min(parallel_limit, shard_count)));
        }
        auto run_shard = [this, &context, &pvs, &pushable_predicates](const std::size_t shard_begin, const std::size_t shard_end)
        {
            dp::service::query::QueryPvStatsRequest request;
            if (pvs.empty())
                request.mutable_pvnamepattern()->set_pattern(pvNamePatternFor(pushable_predicates));
            for (std::size_t index = shard_begin; index < shard_end; ++index)
                request.mutable_pvnamelist()->add_pvnames(pvs[index]);
            auto handle = pool_->acquire();
            auto rpc_context = std::make_shared<grpc::ClientContext>();
            auto cancellation_registration = context.cancellation
                                                 ? context.cancellation->onCancel([rpc_context]
                                                                                  { rpc_context->TryCancel(); })
                                                 : QueryCancellation::Registration{};
            dp::service::query::QueryPvStatsResponse response;
            const auto status = handle->query_stub->queryPvStats(rpc_context.get(), request, &response);
            if (context.cancellation && context.cancellation->cancelled())
                throw QueryCancelled{};
            if (!status.ok())
                throw std::runtime_error("MLDP queryPvStats failed: " + status.error_message());
            if (!response.has_statsresult())
                throw std::runtime_error("MLDP queryPvStats failed: " + response.exceptionalresult().message());
            return StatsShard{.stats = {response.statsresult().pvstats().begin(), response.statsresult().pvstats().end()}};
        };
        std::vector<PvStats> ordered_stats;
        if (pvs.empty())
            ordered_stats = run_shard(0, 0).stats;
        for (std::size_t shard_begin = 0; shard_begin < request_count; shard_begin += shard_size)
        {
            const auto shard_end = std::min(request_count, shard_begin + shard_size);
            futures.push_back(std::async(std::launch::async, run_shard, shard_begin, shard_end));
            if (futures.size() == parallel_limit || shard_end == request_count)
            {
                for (auto& future : futures)
                {
                    auto shard = future.get();
                    ordered_stats.insert(ordered_stats.end(), std::make_move_iterator(shard.stats.begin()), std::make_move_iterator(shard.stats.end()));
                }
                futures.clear();
            }
        }
        if (context.progress)
            context.progress->setParallelShards(0, static_cast<uint64_t>(std::min(parallel_limit, shard_count)));

        arrow::StringBuilder    pv_builder;
        arrow::TimestampBuilder first_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::TimestampBuilder last_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::Int64Builder     buckets_builder;
        for (const auto& stat : ordered_stats)
        {
            if (context.cancellation)
                context.cancellation->throwIfCancelled();
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
        auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()),
                                                             arrow::field("start_time", first->type()),
                                                             arrow::field("end_time", last->type()),
                                                             arrow::field("num_buckets", arrow::int64())}),
                                              pv->length(), {pv, first, last, buckets});
        return materializedStream({std::move(batch)});
    }

    // -----------------------------------------------------------------------
    // mldp.time_series_table — queryTable (wide pivot) or parallel shards
    // -----------------------------------------------------------------------

    const auto pvs = requestedPvs(pushable_predicates);
    if (context.series_per_shard != 0 && pvs.size() > context.series_per_shard)
    {
        const auto shard_size = static_cast<std::size_t>(context.series_per_shard);
        const auto capability_limit = std::max<std::size_t>(1, maxConcurrentStreams());
        const auto parallel_limit = context.max_parallel_requests == 0
                                        ? capability_limit
                                        : std::min<std::size_t>(capability_limit, context.max_parallel_requests);

        struct WideShard
        {
            std::shared_ptr<arrow::RecordBatch> batch;
        };

        const auto                          shard_count = (pvs.size() + shard_size - 1) / shard_size;
        std::vector<std::future<WideShard>> futures;
        std::vector<WideShard>              shards;
        futures.reserve(shard_count);
        if (context.progress)
        {
            context.progress->setActivity(std::string(kTimeSeriesWideTable), "parallel series-shard scan", "querying wide series shards");
            context.progress->setParallelShards(static_cast<uint64_t>(std::min(parallel_limit, shard_count)),
                                                static_cast<uint64_t>(std::min(parallel_limit, shard_count)));
        }
        auto run_shard = [this, &context, &pushable_predicates, &projection_hint, &pvs](const std::size_t begin, const std::size_t end)
        {
            auto predicates = withoutPvListPredicates(pushable_predicates);
            std::vector<ExecutableLiteralValue> shard_pvs;
            for (std::size_t index = begin; index < end; ++index)
                shard_pvs.emplace_back(pvs[index]);
            predicates.push_back(Predicate{.column = "pv", .op = PredicateOp::IN, .values = std::move(shard_pvs)});
            auto shard_context = context;
            shard_context.series_per_shard = 0;
            auto stream = executeStream(kTimeSeriesWideTable, predicates, projection_hint, shard_context);
            std::shared_ptr<arrow::RecordBatch> shard_batch;
            while (auto next_batch = stream->next())
                shard_batch = std::move(next_batch);
            return WideShard{.batch = std::move(shard_batch)};
        };
        for (std::size_t begin = 0; begin < pvs.size(); begin += shard_size)
        {
            const auto end = std::min(pvs.size(), begin + shard_size);
            futures.push_back(std::async(std::launch::async, run_shard, begin, end));
            if (futures.size() == parallel_limit || end == pvs.size())
            {
                try
                {
                    for (auto& future : futures)
                        shards.push_back(future.get());
                }
                catch (...)
                {
                    if (context.cancellation)
                        context.cancellation->requestCancel();
                    for (auto& future : futures)
                        if (future.valid())
                        {
                            try { future.get(); } catch (...) {}
                        }
                    throw;
                }
                futures.clear();
            }
        }
        if (context.progress)
            context.progress->setParallelShards(0, static_cast<uint64_t>(std::min(parallel_limit, shard_count)));

        std::set<int64_t>                                                     all_timestamps;
        std::unordered_map<std::string, std::shared_ptr<arrow::Array>>        values;
        std::unordered_map<std::string, std::unordered_map<int64_t, int64_t>> value_rows;
        for (const auto& shard : shards)
        {
            if (!shard.batch)
                continue;
            const auto times = std::dynamic_pointer_cast<arrow::TimestampArray>(shard.batch->GetColumnByName("time"));
            if (!times)
                throw std::runtime_error("MLDP time_series_table shard has no timestamp column");
            for (int64_t row = 0; row < times->length(); ++row)
                all_timestamps.insert(times->Value(row));
            for (int column = 1; column < shard.batch->num_columns(); ++column)
            {
                const auto& name = shard.batch->schema()->field(column)->name();
                values.emplace(name, shard.batch->column(column));
                auto& rows = value_rows[name];
                for (int64_t row = 0; row < times->length(); ++row)
                    rows.emplace(times->Value(row), row);
            }
        }
        if (all_timestamps.empty())
            return materializedStream({});
        auto*                   pool = context.pool != nullptr ? context.pool : arrow::default_memory_pool();
        arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), pool);
        for (const auto timestamp : all_timestamps)
            if (!time_builder.Append(timestamp).ok())
                throw std::runtime_error("Failed to append merged MLDP timestamp");
        std::shared_ptr<arrow::Array> time;
        if (!time_builder.Finish(&time).ok())
            throw std::runtime_error("Failed to finish merged MLDP timestamp column");
        std::vector<std::shared_ptr<arrow::Field>> fields = {arrow::field("time", time->type())};
        std::vector<std::shared_ptr<arrow::Array>> arrays = {time};
        for (const auto& pv : pvs)
        {
            const auto source = values.find(pv);
            if (source == values.end())
            {
                arrow::NullBuilder builder(pool);
                for (std::size_t index = 0; index < all_timestamps.size(); ++index)
                    if (!builder.AppendNull().ok())
                        throw std::runtime_error("Failed to append merged MLDP null column");
                std::shared_ptr<arrow::Array> array;
                if (!builder.Finish(&array).ok())
                    throw std::runtime_error("Failed to finish merged MLDP null column");
                fields.push_back(arrow::field(pv, array->type(), true));
                arrays.push_back(std::move(array));
                continue;
            }
            std::unique_ptr<arrow::ArrayBuilder> builder;
            const auto                           status = arrow::MakeBuilder(pool, source->second->type(), &builder);
            if (!status.ok())
                throw std::runtime_error(status.ToString());
            for (const auto timestamp : all_timestamps)
            {
                const auto row = value_rows[pv].find(timestamp);
                if (row == value_rows[pv].end())
                {
                    if (!builder->AppendNull().ok())
                        throw std::runtime_error("Failed to append merged MLDP null");
                }
                else
                {
                    const auto scalar = source->second->GetScalar(row->second);
                    if (!scalar.ok() || !builder->AppendScalar(**scalar).ok())
                        throw std::runtime_error("Failed to append merged MLDP value");
                }
            }
            std::shared_ptr<arrow::Array> array;
            if (!builder->Finish(&array).ok())
                throw std::runtime_error("Failed to finish merged MLDP value column");
            fields.push_back(arrow::field(pv, array->type(), true));
            arrays.push_back(std::move(array));
        }
        auto merged = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), time->length(), std::move(arrays));
        return materializedStream({std::move(merged)});
    }

    // Single-shard wide table: backend-paged querySamples RPCs (dp-grpc V2), merged into one wide batch.
    auto spec = buildQuerySpec(pushable_predicates, requestedTimeRange(pushable_predicates), true);

    MldpQuerySamplesPagedStream paged_stream(pool_->acquire(), std::move(spec), context);

    std::vector<int64_t>                                              timestamps_ns;
    std::unordered_map<std::string, dp::service::common::DataColumn> merged_columns;
    while (auto page = paged_stream.next())
    {
        for (const auto& ts : page->timestamplist().timestamps())
            timestamps_ns.push_back(timestampToNanoseconds(ts));
        for (const auto& column : page->datacolumns())
        {
            auto& merged = merged_columns[column.name()];
            if (merged.name().empty())
            {
                merged.set_name(column.name());
                *merged.mutable_metadata() = column.metadata();
            }
            for (const auto& value : column.datavalues())
                *merged.add_datavalues() = value;
        }
    }

    // Explicit PVs keep request order; selector-chosen PVs are sorted by name.
    auto ordered_pvs = pvs;
    if (ordered_pvs.empty())
    {
        for (const auto& [name, column] : merged_columns)
            ordered_pvs.push_back(name);
        std::sort(ordered_pvs.begin(), ordered_pvs.end());
    }
    std::vector<const dp::service::common::DataColumn*> columns;
    columns.reserve(ordered_pvs.size());
    for (const auto& requested_pv : ordered_pvs)
    {
        const auto found = merged_columns.find(requested_pv);
        if (found == merged_columns.end())
            continue;
        if (matchesColumnPredicates(found->second, pushable_predicates))
            columns.push_back(&found->second);
    }

    if (columns.empty())
        return materializedStream({});

    auto*                   pool = context.pool != nullptr ? context.pool : arrow::default_memory_pool();
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
        const auto                           builder_status = arrow::MakeBuilder(pool, type, &builder);
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
    auto wide_batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), time->length(), std::move(arrays));
    return materializedStream({std::move(wide_batch)});
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
