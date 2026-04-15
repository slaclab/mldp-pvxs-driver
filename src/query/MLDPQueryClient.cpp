//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/MLDPQueryClient.h>

#include <util/log/Logger.h>

#include <grpcpp/grpcpp.h>
#include <query.grpc.pb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::log;
using mldp_pvxs_driver::util::pool::MLDPGrpcQueryPool;

namespace {

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
        SourceTimestamp last  = first;
        for (int i = 1; i < list.timestamps_size(); ++i)
        {
            const SourceTimestamp current = makeSourceTimestamp(list.timestamps(i));
            if (isBefore(current, first)) first = current;
            if (isBefore(last, current))  last  = current;
        }
        return std::make_pair(first, last);
    }
    if (data_timestamps.has_samplingclock())
    {
        const auto& clock = data_timestamps.samplingclock();
        if (!clock.has_starttime()) return std::nullopt;
        const SourceTimestamp first = makeSourceTimestamp(clock.starttime());
        SourceTimestamp       last  = first;
        const auto count       = static_cast<uint64_t>(clock.count());
        const auto period_nanos = clock.periodnanos();
        if (count > 1 && period_nanos > 0)
        {
            const auto steps        = count - 1;
            const auto offset_nanos = static_cast<unsigned __int128>(steps) *
                                      static_cast<unsigned __int128>(period_nanos);
            const auto add_secs  = static_cast<uint64_t>(offset_nanos / 1'000'000'000ULL);
            const auto add_nanos = static_cast<uint64_t>(offset_nanos % 1'000'000'000ULL);
            last.epoch_seconds += add_secs;
            last.nanoseconds   += add_nanos;
            if (last.nanoseconds >= 1'000'000'000ULL)
            {
                last.epoch_seconds += 1;
                last.nanoseconds   -= 1'000'000'000ULL;
            }
        }
        return std::make_pair(first, last);
    }
    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MLDPQueryClient::MLDPQueryClient(const util::pool::MLDPGrpcPoolConfig& poolConfig,
                                 std::shared_ptr<metrics::Metrics>     metrics)
    : logger_(makeQueryClientLogger())
    , pool_(MLDPGrpcQueryPool::create(poolConfig, std::move(metrics)))
{
}

// ---------------------------------------------------------------------------
// querySourcesInfo
// ---------------------------------------------------------------------------

std::vector<IDataBus::SourceInfo>
MLDPQueryClient::querySourcesInfo(const std::set<std::string>& source_names)
{
    std::vector<IDataBus::SourceInfo> infos;
    if (source_names.empty()) return infos;

    try
    {
        auto  handle     = pool_->acquire();
        auto* query_stub = handle->query_stub.get();
        if (!query_stub)
        {
            handle->query_stub = handle->makeQueryStub();
            query_stub         = handle->query_stub.get();
        }
        if (!query_stub)
        {
            errorf(*logger_, "Failed to create query stub for source metadata request");
            return infos;
        }

        dp::service::query::QueryPvMetadataRequest request;
        auto* pv_name_list = request.mutable_pvnamelist();
        pv_name_list->mutable_pvnames()->Reserve(static_cast<int>(source_names.size()));
        for (const auto& source : source_names)
        {
            if (!source.empty()) pv_name_list->add_pvnames(source);
        }
        if (pv_name_list->pvnames().empty()) return infos;

        grpc::ClientContext                         context;
        dp::service::query::QueryPvMetadataResponse response;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        const auto status = query_stub->queryPvMetadata(&context, request, &response);

        if (!status.ok())
        {
            const bool metadata_rpc_missing =
                status.error_code() == grpc::StatusCode::UNIMPLEMENTED ||
                status.error_message().find("Method not found") != std::string::npos;
            if (!metadata_rpc_missing)
            {
                errorf(*logger_, "queryPvMetadata RPC failed: {}", status.error_message());
                return infos;
            }

            warnf(*logger_,
                  "queryPvMetadata unavailable ({}). Falling back to queryData-derived timestamps.",
                  status.error_message());

            dp::service::query::QueryDataRequest data_request;
            auto* spec = data_request.mutable_queryspec();
            for (const auto& source : source_names)
            {
                if (!source.empty()) spec->add_pvnames(source);
            }
            if (spec->pvnames().empty()) return infos;

            auto* begin_ts = spec->mutable_begintime();
            begin_ts->set_epochseconds(0);
            auto* end_ts = spec->mutable_endtime();
            end_ts->set_epochseconds(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()) + 1);

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
                if (pvname.empty() || !source_names.contains(pvname)) continue;

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
                if (!bucket.has_datatimestamps()) continue;

                const auto range = extractTimestampRange(bucket.datatimestamps());
                if (!range.has_value()) continue;

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
                    info.last_bucket_sample_period        = clock.periodnanos();
                    info.last_bucket_sample_count         = clock.count();
                    info.last_bucket_data_timestamps_type = "SAMPLING_CLOCK";
                }
                else if (data_timestamps.has_timestamplist())
                {
                    info.last_bucket_sample_count         =
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
            errorf(*logger_, "queryPvMetadata returned exceptional result: {}",
                   response.exceptionalresult().message());
            return infos;
        }
        if (!response.has_metadataresult()) return infos;

        const auto& pv_infos = response.metadataresult().pvinfos();
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
MLDPQueryClient::querySourcesData(const std::set<std::string>&     source_names,
                                   const QuerySourcesDataOptions&   options)
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
        auto  handle     = pool_->acquire();
        auto* query_stub = handle->query_stub.get();
        if (!query_stub)
        {
            handle->query_stub = handle->makeQueryStub();
            query_stub         = handle->query_stub.get();
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
            auto* spec = request.mutable_queryspec();
            for (const auto& source : source_names)
            {
                if (!source.empty()) spec->add_pvnames(source);
            }
            if (spec->pvnames().empty())
            {
                return std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>{};
            }

            const auto now   = std::chrono::system_clock::now();
            const auto begin = now - options.lookback_window;
            const auto end   = now + options.forward_window;
            auto* begin_ts   = spec->mutable_begintime();
            begin_ts->set_epochseconds(
                std::chrono::duration_cast<std::chrono::seconds>(begin.time_since_epoch()).count());
            auto* end_ts = spec->mutable_endtime();
            end_ts->set_epochseconds(
                std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()).count());

            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + options.rpc_deadline);

            dp::service::query::QueryDataResponse response;
            const auto status = query_stub->queryData(&context, request, &response);
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
                if (collected.size() == source_names.size()) return collected;
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
