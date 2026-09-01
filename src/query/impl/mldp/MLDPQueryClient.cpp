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
    if (table_name == "mldp.time_series" || table_name == "mldp.time_series_table")
    {
        const bool                wide_table = table_name == "mldp.time_series_table";
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
        schema.emplace_back("window", ColumnType::TIMESTAMP, false, false, std::set<PredicateOp>{PredicateOp::IN}, std::set<PredicateOp>{},
                            "Time-series interval input; accepts window IN (start, end) or window IN (SELECT time, end_time ...)");
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

std::vector<std::string> requestedPvs(const std::vector<Predicate>& predicates)
{
    std::vector<std::string> names;
    std::set<std::string>    seen;
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

bool matchesColumnPredicates(const dp::service::common::DataColumn& column,
                             const std::vector<Predicate>&          predicates)
{
    return matchesColumnMetadataPredicates(column.metadata(), dataValuesKind(column.datavalues()), predicates);
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
    // mldp.time_series — native bidi stream; drain all batches, spill, replay
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
            const auto [begin, end] = requestedTimeRange(pushable_predicates);
            dp::service::query::QueryDataRequest request;
            auto*                                spec = request.mutable_queryspec();
            setTimestamp(spec->mutable_begintime(), begin);
            setTimestamp(spec->mutable_endtime(), end);
            for (const auto& pv : pvs)
                spec->add_pvnames(pv);
            raw_stream = std::make_unique<MldpBidiRecordBatchStream>(pool_->acquire(), std::move(request), pushable_predicates, projection_hint, context);
        }
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        while (auto batch = raw_stream->next())
            batches.push_back(std::move(batch));
        return materializedStream(std::move(batches));
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
        auto run_shard = [this, &context, &pvs](const std::size_t shard_begin, const std::size_t shard_end)
        {
            dp::service::query::QueryPvStatsRequest request;
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
                                                             arrow::field("first_timestamp", first->type()),
                                                             arrow::field("last_timestamp", last->type()),
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
            auto predicates = pushable_predicates;
            predicates.erase(std::remove_if(predicates.begin(), predicates.end(), [](const Predicate& predicate)
                                            {
                                                return predicate.column == "pv";
                                            }),
                             predicates.end());
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

    // Single-shard wide table: queryTable RPC
    const auto [begin, end] = requestedTimeRange(pushable_predicates);
    dp::service::query::QueryTableRequest request;
    request.set_format(dp::service::query::QueryTableRequest::TABLE_FORMAT_COLUMN);
    setTimestamp(request.mutable_begintime(), begin);
    setTimestamp(request.mutable_endtime(), end);
    for (const auto& pv_name : pvs)
        request.mutable_pvnamelist()->add_pvnames(pv_name);

    auto handle = pool_->acquire();
    auto rpc_context = std::make_shared<grpc::ClientContext>();
    dp::service::query::QueryTableResponse response;
    auto cancellation_registration = context.cancellation
                                         ? context.cancellation->onCancel([rpc_context]
                                                                          { rpc_context->TryCancel(); })
                                         : QueryCancellation::Registration{};
    if (context.cancellation)
        context.cancellation->throwIfCancelled();
    const auto status = handle->query_stub->queryTable(rpc_context.get(), request, &response);
    if (context.cancellation && context.cancellation->cancelled())
        throw QueryCancelled{};
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
