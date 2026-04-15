//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "util/log/Logger.h"
#include <controller/MLDPPVXSController.h>
#include <memory>
#include <reader/ReaderFactory.h>
#include <util/StringFormat.h>
#include <writer/grpc/MLDPGrpcWriter.h>

#ifdef MLDP_PVXS_HDF5_ENABLED
#include <writer/hdf5/HDF5Writer.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <ranges>
#include <set>
#include <stdexcept>
#include <thread>

using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;

using mldp_pvxs_driver::util::pool::MLDPGrpcQueryPool;

namespace {
// Creates a dedicated logger for controller lifecycle, bus, and query operations.
std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeControllerLogger()
{
    std::string loggerName = "controlller";
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}

using dp::service::common::DataTimestamps;
using dp::service::common::Timestamp;

SourceTimestamp makeSourceTimestamp(const Timestamp& ts)
{
    return SourceTimestamp{
        ts.epochseconds(),
        ts.nanoseconds()};
}

bool isBefore(const SourceTimestamp& lhs, const SourceTimestamp& rhs)
{
    if (lhs.epoch_seconds != rhs.epoch_seconds)
    {
        return lhs.epoch_seconds < rhs.epoch_seconds;
    }
    return lhs.nanoseconds < rhs.nanoseconds;
}

std::optional<std::pair<SourceTimestamp, SourceTimestamp>> extractTimestampRange(const DataTimestamps& data_timestamps)
{
    // Metadata/query responses may carry timestamps either as an explicit list
    // or as sampling-clock metadata. Normalize both formats into a [first,last]
    // range so callers can merge bucket boundaries consistently.
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
            {
                first = current;
            }
            if (isBefore(last, current))
            {
                last = current;
            }
        }
        return std::make_pair(first, last);
    }

    if (data_timestamps.has_samplingclock())
    {
        const auto& clock = data_timestamps.samplingclock();
        if (!clock.has_starttime())
        {
            return std::nullopt;
        }

        const SourceTimestamp first = makeSourceTimestamp(clock.starttime());
        SourceTimestamp       last = first;
        const auto            count = static_cast<uint64_t>(clock.count());
        const auto            period_nanos = clock.periodnanos();
        if (count > 1 && period_nanos > 0)
        {
            const auto steps = count - 1;
            const auto offset_nanos = static_cast<unsigned __int128>(steps) * static_cast<unsigned __int128>(period_nanos);
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

std::shared_ptr<MLDPPVXSController> MLDPPVXSController::create(const config::Config& config)
{
    return std::shared_ptr<MLDPPVXSController>(new MLDPPVXSController(config));
}

MLDPPVXSController::MLDPPVXSController(const config::Config& config)
    : logger_(makeControllerLogger())
    , config_(config)
    , thread_pool_(std::make_shared<BS::light_thread_pool>(config_.controllerThreadPoolSize()))
    , metrics_(std::make_shared<metrics::Metrics>(*config_.metricsConfig()))
    , running_(false)
{
    // Constructor implementation
}

MLDPPVXSController::~MLDPPVXSController()
{
    if (running_.load())
    {
        stop();
    }
    thread_pool_.reset();
    mldp_query_pool_.reset();
    metrics_.reset();
}

void MLDPPVXSController::start()
{
    if (running_.load())
    {
        warnf(*logger_, "Controller is already started");
        return;
    }

    running_.store(true);
    infof(*logger_, "Controller is starting");

    // -- Build writers from config --
    const auto& writerCfg = config_.writerConfig();

    if (writerCfg.grpcEnabled && writerCfg.grpcConfig.has_value())
    {
        auto w = std::make_unique<MLDPGrpcWriter>(writerCfg.grpcConfig.value(), metrics_);
        w->start();
        writers_.push_back(std::move(w));
    }

#ifdef MLDP_PVXS_HDF5_ENABLED
    if (writerCfg.hdf5Enabled && writerCfg.hdf5Config.has_value())
    {
        auto w = std::make_unique<HDF5Writer>(writerCfg.hdf5Config.value());
        w->start();
        writers_.push_back(std::move(w));
    }
#endif

    if (writers_.empty())
    {
        running_.store(false);
        throw std::runtime_error("Controller: no writers were started");
    }

    // -- Query pool (used by querySourcesInfo / querySourcesData) --
    // Re-use the pool config from the gRPC writer config when available,
    // otherwise skip (HDF5-only mode has no query pool).
    if (writerCfg.grpcEnabled && writerCfg.grpcConfig.has_value())
    {
        mldp_query_pool_ = MLDPGrpcQueryPool::create(writerCfg.grpcConfig->poolConfig, metrics_);
    }

    // -- Readers --
    infof(*logger_, "Starting readers");
    for (const auto& entry : config_.readerEntries())
    {
        const auto& type = entry.first;
        const auto& readerConfig = entry.second;
        auto        reader = ReaderFactory::create(type, shared_from_this(), readerConfig, metrics_);
        readers_.push_back(std::move(reader));
    }
    infof(*logger_, "Controller started");
}

void MLDPPVXSController::stop()
{
    if (!running_.load())
    {
        warnf(*logger_, "Controller already stopped");
        return;
    }
    infof(*logger_, "Controller is stopping");
    running_.store(false);

    readers_.clear();

    for (auto& w : writers_)
    {
        w->stop();
    }
    writers_.clear();

    infof(*logger_, "Controller stopped");
}

bool MLDPPVXSController::push(EventBatch batch_values)
{
    if (!running_.load())
    {
        return false;
    }

    if (batch_values.root_source.empty())
    {
        warnf(*logger_, "Received batch with empty root source, skipping push.");
        return false;
    }

    if (batch_values.frames.empty())
    {
        warnf(*logger_, "Received empty batch for root source {}, skipping push.", batch_values.root_source);
        return false;
    }

    // Best-effort fan-out: copy for all-but-last writer; move for the last.
    bool anyAccepted = false;
    const std::size_t n = writers_.size();
    // Save root_source before the loop: std::move on the last iteration
    // would leave batch_values.root_source in a valid-but-unspecified state.
    const std::string rootSource = batch_values.root_source;
    for (std::size_t i = 0; i < n; ++i)
    {
        bool ok = false;
        if (i + 1 < n)
        {
            // Copy for all writers except the last
            ok = writers_[i]->push(batch_values);
        }
        else
        {
            // Move for the last writer
            ok = writers_[i]->push(std::move(batch_values));
        }
        if (!ok)
        {
            warnf(*logger_, "Writer '{}' rejected batch for source {}",
                  writers_[i]->name(), rootSource);
        }
        anyAccepted = anyAccepted || ok;
    }
    return anyAccepted;
}

std::vector<IDataBus::SourceInfo> MLDPPVXSController::querySourcesInfo(const std::set<std::string>& source_names)
{
    // Preferred path is queryPvMetadata (direct metadata API).
    // Compatibility fallback (older servers) is queryData + timestamp range
    // extraction from returned buckets.
    std::vector<IDataBus::SourceInfo> infos;
    if (source_names.empty())
    {
        return infos;
    }
    if (!mldp_query_pool_)
    {
        warnf(*logger_, "querySourcesInfo called before MLDP query pool initialization");
        return infos;
    }

    try
    {
        mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle = mldp_query_pool_->acquire();
        auto*                                                query_stub = handle->query_stub.get();
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

        dp::service::query::QueryPvMetadataRequest request;
        auto*                                      pv_name_list = request.mutable_pvnamelist();
        pv_name_list->mutable_pvnames()->Reserve(static_cast<int>(source_names.size()));
        for (const auto& source : source_names)
        {
            if (!source.empty())
            {
                pv_name_list->add_pvnames(source);
            }
        }
        if (pv_name_list->pvnames().empty())
        {
            return infos;
        }

        grpc::ClientContext                         context;
        dp::service::query::QueryPvMetadataResponse response;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        const auto status = query_stub->queryPvMetadata(&context, request, &response);
        if (!status.ok())
        {
            const bool metadata_rpc_missing = status.error_code() == grpc::StatusCode::UNIMPLEMENTED ||
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
            auto*                                spec = data_request.mutable_queryspec();
            for (const auto& source : source_names)
            {
                if (!source.empty())
                {
                    spec->add_pvnames(source);
                }
            }
            if (spec->pvnames().empty())
            {
                return infos;
            }
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

            // Merge bucket-level observations into one SourceInfo per pvname.
            std::unordered_map<std::string, IDataBus::SourceInfo> merged_infos;
            for (const auto& bucket : data_response.querydata().databuckets())
            {
                const auto& pvname = bucket.pvname();
                if (pvname.empty() || !source_names.contains(pvname))
                {
                    continue;
                }

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
                {
                    continue;
                }
                const auto range = extractTimestampRange(bucket.datatimestamps());
                if (!range.has_value())
                {
                    continue;
                }

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
                    info.last_bucket_sample_count = static_cast<uint32_t>(data_timestamps.timestamplist().timestamps_size());
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
            errorf(*logger_, "queryPvMetadata returned exceptional result: {}", response.exceptionalresult().message());
            return infos;
        }
        if (!response.has_metadataresult())
        {
            return infos;
        }

        const auto& pv_infos = response.metadataresult().pvinfos();
        infos.reserve(static_cast<std::size_t>(pv_infos.size()));
        for (const auto& pv_info : pv_infos)
        {
            IDataBus::SourceInfo info;
            info.source_name = pv_info.pvname();

            if (pv_info.has_firstdatatimestamp())
            {
                info.first_timestamp = makeSourceTimestamp(pv_info.firstdatatimestamp());
            }
            if (pv_info.has_lastdatatimestamp())
            {
                info.last_timestamp = makeSourceTimestamp(pv_info.lastdatatimestamp());
            }
            if (!pv_info.lastproviderid().empty())
            {
                info.last_provider_id = pv_info.lastproviderid();
            }
            if (!pv_info.lastprovidername().empty())
            {
                info.last_provider_name = pv_info.lastprovidername();
            }
            if (!pv_info.lastbucketid().empty())
            {
                info.last_bucket_id = pv_info.lastbucketid();
            }
            if (!pv_info.lastbucketdatatype().empty())
            {
                info.last_bucket_data_type = pv_info.lastbucketdatatype();
            }
            if (!pv_info.lastbucketdatatimestampstype().empty())
            {
                info.last_bucket_data_timestamps_type = pv_info.lastbucketdatatimestampstype();
            }
            if (pv_info.lastbucketsampleperiod() > 0)
            {
                info.last_bucket_sample_period = pv_info.lastbucketsampleperiod();
            }
            if (pv_info.lastbucketsamplecount() > 0)
            {
                info.last_bucket_sample_count = pv_info.lastbucketsamplecount();
            }
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

std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>> MLDPPVXSController::querySourcesData(
    const std::set<std::string>&              source_names,
    const util::bus::QuerySourcesDataOptions& options)
{
    // queryData is retried until timeout because read-after-write visibility is
    // eventually consistent on some deployments. Success requires at least one
    // collected bucket for every requested source.
    if (source_names.empty())
    {
        return std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>{};
    }
    if (!mldp_query_pool_)
    {
        warnf(*logger_, "querySourcesData called before MLDP query pool initialization");
        return std::nullopt;
    }
    if (options.timeout <= std::chrono::milliseconds::zero())
    {
        warnf(*logger_, "querySourcesData timeout must be > 0");
        return std::nullopt;
    }

    try
    {
        mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject> handle = mldp_query_pool_->acquire();
        auto*                                                query_stub = handle->query_stub.get();
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
                {
                    spec->add_pvnames(source);
                }
            }
            if (spec->pvnames().empty())
            {
                return std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>{};
            }

            const auto now = std::chrono::system_clock::now();
            const auto begin = now - options.lookback_window;
            const auto end = now + options.forward_window;
            auto*      begin_ts = spec->mutable_begintime();
            begin_ts->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(begin.time_since_epoch()).count());
            auto* end_ts = spec->mutable_endtime();
            end_ts->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()).count());

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
                    {
                        continue;
                    }

                    collected[pvname].push_back(bucket.datavalues());
                }

                if (collected.size() == source_names.size())
                {
                    return collected;
                }
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

Metrics& MLDPPVXSController::metrics() const
{
    if (!metrics_)
    {
        throw std::runtime_error("Metrics not configured for controller");
    }
    return *metrics_;
}
