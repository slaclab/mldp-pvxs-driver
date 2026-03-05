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
#include <google/protobuf/arena.h>
#include <memory>
#include <reader/ReaderFactory.h>
#include <util/StringFormat.h>

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

using mldp_pvxs_driver::util::pool::MLDPGrpcIngestionePool;
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

bool hasTimestampList(const dp::service::common::DataFrame& frame)
{
    return frame.has_datatimestamps() &&
           frame.datatimestamps().has_timestamplist() &&
           frame.datatimestamps().timestamplist().timestamps_size() > 0;
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
    // Destructor implementation
    if (running_.load())
    {
        stop();
    }
    // clear pools
    thread_pool_.reset();
    mldp_ingestion_pool_.reset();
    mldp_query_pool_.reset();
    // clear metrics
    metrics_.reset();
}

void MLDPPVXSController::start()
{
    if (running_.load())
    {
        warnf(*logger_, "Controller is already started");
        return;
    }

    // Start order matters:
    // 1) mark running
    // 2) initialize ingestion/query pools and register provider
    // 3) spawn workers
    // 4) construct readers (readers can immediately push to this bus)
    running_.store(true);
    infof(*logger_, "Controller is starting");
    // Start allocating mldp pool (constructor registers provider)
    mldp_ingestion_pool_ = MLDPGrpcIngestionePool::create(config_.pool(), metrics_);
    mldp_query_pool_ = MLDPGrpcQueryPool::create(config_.pool(), metrics_);
    provider_id_ = mldp_ingestion_pool_->providerId();
    if (provider_id_.empty())
    {
        running_ = false;
        throw std::runtime_error("Failed to register provider with MLDP ingestion service");
    }

    const std::size_t worker_count = std::max<std::size_t>(1, static_cast<std::size_t>(config_.controllerThreadPoolSize()));
    next_channel_.store(0, std::memory_order_relaxed);
    queued_items_.store(0);
    channels_.clear();
    channels_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        channels_.push_back(std::make_unique<WorkerChannel>());
    }
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        thread_pool_->detach_task([this, i]()
                                  {
                                      workerLoop(i);
                                  });
    }

    // Start readers (dispatch based on declared reader type)
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
    if (running_.load() == false)
    {
        warnf(*logger_, "Controller already stopped");
        return;
    }
    infof(*logger_, "Controller is stopping");
    // Stop order:
    // 1) reject new pushes
    // 2) tear down readers so no new events are produced
    // 3) signal worker channels and wait for drain/exit
    running_.store(false);
    // clear readers
    readers_.clear();
    // Signal all worker channels to shut down
    for (auto& ch : channels_)
    {
        {
            std::lock_guard lk(ch->mutex);
            ch->shutdown = true;
        }
        ch->cv.notify_one();
    }
    // Wait for all worker tasks to complete
    thread_pool_->wait();
    channels_.clear();
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

    auto tags = std::make_shared<const std::vector<std::string>>(batch_values.tags);

    // Distribute frames in round-robin order and enqueue directly to worker
    // queues to avoid building a temporary per-channel structure.
    bool enqueued = false;
    for (auto& frame : batch_values.frames)
    {
        if (!hasTimestampList(frame))
        {
            errorf(*logger_, "Dropping frame for root source {}: missing DataFrame.datatimestamps.timestamplist", batch_values.root_source);
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0, {{"source", batch_values.root_source}});
                        });
            continue;
        }

        const auto idx = next_channel_.fetch_add(1, std::memory_order_relaxed) % channels_.size();
        QueueItem  item{
            batch_values.root_source,
            tags,
            std::move(frame)};
        {
            std::lock_guard lk(channels_[idx]->mutex);
            channels_[idx]->items.push_back(std::move(item));
        }
        channels_[idx]->cv.notify_one();
        queued_items_.fetch_add(1, std::memory_order_relaxed);
        enqueued = true;
    }

    updateQueueDepthMetric();
    return enqueued;
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
        util::pool::PooledHandle<util::pool::MLDPGrpcObject> handle = mldp_query_pool_->acquire();
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
        util::pool::PooledHandle<util::pool::MLDPGrpcObject> handle = mldp_query_pool_->acquire();
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

void MLDPPVXSController::workerLoop(std::size_t worker_index)
{
    // Worker threads block on the shared queue, build per-source ingestion
    // requests, and write them into a gRPC client-streaming RPC. Each worker
    // keeps a single stream open at a time and flushes (closes/reopens) when
    // byte or age thresholds are reached or when a write fails.
    auto&                                                                          ch = *channels_[worker_index];
    std::optional<util::pool::PooledHandle<util::pool::MLDPGrpcObject>>            handle;
    std::unique_ptr<grpc::ClientWriter<dp::service::ingestion::IngestDataRequest>> writer;
    std::unique_ptr<grpc::ClientContext>                                           context;
    dp::service::ingestion::IngestDataStreamResponse                               response;
    std::chrono::steady_clock::time_point                                          stream_start;
    std::size_t                                                                    stream_payload_bytes = 0;
    std::uint64_t                                                                  request_counter = 0;

    // Gracefully finalizes the current stream and releases pooled handle/stub
    // ownership. Safe to call repeatedly.
    const auto close_stream = [&](const char* reason)
    {
        if (!writer)
        {
            return;
        }
        writer->WritesDone();
        auto    status = writer->Finish();
        int64_t requested_requests = static_cast<int64_t>(request_counter);
        if (status.ok())
        {
            if (response.has_ingestdatastreamresult())
            {
                const auto& result = response.ingestdatastreamresult();
                if (result.numrequests() < 0)
                {
                    errorf(*logger_, "Ingestion stream finished with invalid numrequests ({}): {}", reason, result.numrequests());
                }
                else
                {
                    const char* status_str = nullptr;
                    if (result.numrequests() < requested_requests)
                    {
                        status_str = "[INCOMPLETE - lost or skipped requests]";
                        errorf(*logger_, "Ingestion stream finished with incomplete requests ({}): server accepted {} of {} sent",
                               reason, result.numrequests(), requested_requests);
                    }
                    else if (result.numrequests() > requested_requests)
                    {
                        status_str = "[SERVER ERROR - extra requests acknowledged]";
                        errorf(*logger_, "Ingestion stream finished with mismatch ({}): server reports {} but we sent {}",
                               reason, result.numrequests(), requested_requests);
                    }
                    else
                    {
                        status_str = "OK";
                        tracef(*logger_, "Ingestion stream finished successfully ({}): {} requests", reason, result.numrequests());
                    }
                    if (!status_str)
                    {
                        tracef(*logger_, "Ingestion stream finished successfully ({}): {} requests", reason, result.numrequests());
                    }
                    else
                    {
                        warnf(*logger_, "{}: {}", status_str, reason);
                    }
                }
            }
            if (response.has_exceptionalresult())
            {
                const auto& result = response.exceptionalresult();
                errorf(*logger_, "Ingestion stream finished with exceptional result ({}): {}", reason, result.message());
                metric_call(metrics_, [&](auto& m)
                            {
                                m.incrementBusFailures(1.0, {{"source", "unknown"}});
                            });
            }
        }
        else
        {
            errorf(*logger_, "Ingestion stream finished with error ({}): {}", reason, status.error_message());
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0, {{"source", "unknown"}});
                        });
        }
        writer.reset();
        handle.reset();
        context.reset();
        stream_payload_bytes = 0;
    };

    // Lazily opens a stream when work appears. Worker holds one active stream
    // at a time to amortize connection overhead across many frames.
    const auto ensure_stream = [&]()
    {
        if (writer)
        {
            return true;
        }
        try
        {
            context = std::make_unique<grpc::ClientContext>();
            response = dp::service::ingestion::IngestDataStreamResponse();
            handle.emplace(mldp_ingestion_pool_->acquire());
            writer = (*handle)->stub->ingestDataStream(context.get(), &response);
            if (!writer)
            {
                errorf(*logger_, "Failed to open ingestion stream for queued events");
                metric_call(metrics_, [&](auto& m)
                            {
                                m.incrementBusFailures(1.0, {{"source", "unknown"}});
                            });
                handle.reset();
                context.reset();
                return false;
            }
            stream_start = std::chrono::steady_clock::now();
            stream_payload_bytes = 0;
            return true;
        }
        catch (const std::exception& ex)
        {
            errorf(*logger_, "Failed to acquire ingestion stream: {}", ex.what());
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0, {{"source", "unknown"}});
                        });
            handle.reset();
            writer.reset();
            return false;
        }
    };

    // Wait window doubles as an idle stream flush interval.
    const auto dequeue_timeout = config_.controllerStreamMaxAge();
    while (true)
    {
        QueueItem item;
        bool      has_item = false;
        {
            std::unique_lock lk(ch.mutex);
            ch.cv.wait_for(lk, dequeue_timeout, [&]
                           {
                               return !ch.items.empty() || ch.shutdown;
                           });
            if (ch.shutdown && ch.items.empty())
            {
                lk.unlock();
                break;
            }
            if (!ch.items.empty())
            {
                item = std::move(ch.items.front());
                ch.items.pop_front();
                has_item = true;
            }
        }
        if (!has_item)
        {
            // No new work: close an old idle stream so bytes/age thresholds are
            // enforced even during sparse traffic.
            if (writer)
            {
                const auto elapsed = std::chrono::steady_clock::now() - stream_start;
                if (elapsed >= config_.controllerStreamMaxAge())
                {
                    close_stream("stream age exceeded (idle)");
                }
            }
            continue;
        }

        queued_items_.fetch_sub(1, std::memory_order_relaxed);
        updateQueueDepthMetric();

        const auto item_start = std::chrono::steady_clock::now();
        const auto record_send_time = [this, item_start](prometheus::Labels tags)
        {
            const auto   elapsed = std::chrono::steady_clock::now() - item_start;
            const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
            metric_call(metrics_, [&](auto& m)
                        {
                            m.observeControllerSendTimeSeconds(elapsed_seconds, std::move(tags));
                        });
        };

        if (!ensure_stream())
        {
            record_send_time({{"source", "unknown"}});
            continue;
        }

        const auto elapsed = std::chrono::steady_clock::now() - stream_start;
        if (elapsed >= config_.controllerStreamMaxAge())
        {
            close_stream("stream age exceeded");
            if (!ensure_stream())
            {
                record_send_time({{"source", "unknown"}});
                continue;
            }
        }

        std::size_t accepted_events_total = 0;
        std::size_t payload_bytes_total = 0;

        const auto frame_elapsed = std::chrono::steady_clock::now() - stream_start;
        if (frame_elapsed >= config_.controllerStreamMaxAge())
        {
            close_stream("stream age exceeded");
            if (!ensure_stream())
            {
                record_send_time({{"source", "unknown"}});
                continue;
            }
        }

        google::protobuf::Arena arena;
        auto*                   request = google::protobuf::Arena::CreateMessage<dp::service::ingestion::IngestDataRequest>(&arena);
        std::size_t             accepted_events = 0;
        std::size_t             payload_bytes = 0;
        const auto              request_id = util::format_string("pv_stream_{}_{}_{}", stream_start.time_since_epoch().count(), item.root_source, request_counter++);
        if (!buildRequest(item.root_source, item.frame, request_id, *request, accepted_events, payload_bytes))
        {
            continue;
        }

        if ((stream_payload_bytes + payload_bytes) > config_.controllerStreamMaxBytes() && stream_payload_bytes > 0)
        {
            close_stream("max bytes exceeded");
            if (!ensure_stream())
            {
                record_send_time({{"source", "unknown"}});
                continue;
            }
        }

        if (!writer->Write(*request))
        {
            errorf(*logger_, "Failed to write source {} event to ingestion stream", item.root_source);
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0, {{"source", item.root_source}});
                        });
            close_stream("write failed");
            continue;
        }

        stream_payload_bytes += payload_bytes;
        accepted_events_total += accepted_events;
        payload_bytes_total += payload_bytes;

        if (accepted_events_total > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusPushes(static_cast<double>(accepted_events_total), {{"source", item.root_source}});
                        });
        }
        if (payload_bytes_total > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusPayloadBytes(static_cast<double>(payload_bytes_total), {{"source", item.root_source}});
                        });
            const auto   item_elapsed = std::chrono::steady_clock::now() - item_start;
            const double elapsed_milliseconds = std::chrono::duration<double, std::milli>(item_elapsed).count();
            if (elapsed_milliseconds > 0.0)
            {
                const double bytes_per_second = (static_cast<double>(payload_bytes_total) * 1000.0) / elapsed_milliseconds;
                metric_call(metrics_, [&](auto& m)
                            {
                                m.setBusPayloadBytesPerSecond(bytes_per_second, {{"source", item.root_source}});
                            });
            }
        }
        record_send_time({{"source", item.root_source}});

        const auto post_elapsed = std::chrono::steady_clock::now() - stream_start;
        if (post_elapsed >= config_.controllerStreamMaxAge() || stream_payload_bytes >= config_.controllerStreamMaxBytes())
        {
            close_stream("threshold reached");
        }
    }

    close_stream("shutdown");
}

bool MLDPPVXSController::buildRequest(const std::string&                         source_name,
                                      const dp::service::common::DataFrame&      frame,
                                      const std::string&                         request_id,
                                      dp::service::ingestion::IngestDataRequest& request,
                                      std::size_t&                               accepted_events,
                                      std::size_t&                               payload_bytes)
{
    // Build a self-contained request and validate minimum ingestion contract:
    // - at least one populated column
    // - timestamp list present
    // Callers rely on false to skip bad frames without failing the worker loop.
    request.set_providerid(provider_id_);
    request.set_clientrequestid(request_id);

    auto* data_frame = request.mutable_ingestiondataframe();
    *data_frame = frame;

    const bool has_columns = data_frame->doublecolumns_size() > 0 || data_frame->floatcolumns_size() > 0 ||
                             data_frame->datacolumns_size() > 0 || data_frame->int32columns_size() > 0 ||
                             data_frame->int64columns_size() > 0 || data_frame->boolcolumns_size() > 0 ||
                             data_frame->stringcolumns_size() > 0 || data_frame->enumcolumns_size() > 0 ||
                             data_frame->imagecolumns_size() > 0 || data_frame->structcolumns_size() > 0 ||
                             data_frame->doublearraycolumns_size() > 0 || data_frame->floatarraycolumns_size() > 0 ||
                             data_frame->int32arraycolumns_size() > 0 || data_frame->int64arraycolumns_size() > 0 ||
                             data_frame->boolarraycolumns_size() > 0;
    if (!has_columns)
    {
        warnf(*logger_, "No valid columns for source {}, skipping request", source_name);
        return false;
    }

    if (!hasTimestampList(*data_frame))
    {
        errorf(*logger_, "Dropping frame for source {}: missing DataFrame.datatimestamps.timestamplist", source_name);
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementBusFailures(1.0, {{"source", source_name}});
                    });
        return false;
    }

    accepted_events = static_cast<std::size_t>(data_frame->datatimestamps().timestamplist().timestamps_size());
    payload_bytes = static_cast<std::size_t>(request.ByteSizeLong());
    return true;
}

Metrics& MLDPPVXSController::metrics() const
{
    if (!metrics_)
    {
        throw std::runtime_error("Metrics not configured for controller");
    }
    return *metrics_;
}

void MLDPPVXSController::updateQueueDepthMetric()
{
    const double depth = static_cast<double>(queued_items_.load(std::memory_order_relaxed));
    metric_call(metrics_, [&](auto& m)
                {
                    m.setControllerQueueDepth(depth);
                });
}
