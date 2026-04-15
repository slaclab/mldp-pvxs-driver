//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/grpc/MLDPGrpcWriter.h>

#include <util/StringFormat.h>
#include <util/log/Logger.h>

#include <google/protobuf/arena.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <thread>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;

using mldp_pvxs_driver::util::pool::MLDPGrpcIngestionePool;

namespace {

std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeWriterLogger()
{
    std::string n = "grpc_writer";
    return mldp_pvxs_driver::util::log::newLogger(n);
}

bool hasTimestampListW(const dp::service::common::DataFrame& frame)
{
    return frame.has_datatimestamps() &&
           frame.datatimestamps().has_timestamplist() &&
           frame.datatimestamps().timestamplist().timestamps_size() > 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MLDPGrpcWriter::MLDPGrpcWriter(MLDPGrpcWriterConfig config,
                               std::shared_ptr<Metrics> metrics)
    : config_(std::move(config))
    , logger_(makeWriterLogger())
    , metrics_(std::move(metrics))
{
}

MLDPGrpcWriter::~MLDPGrpcWriter()
{
    if (running_.load())
    {
        stop();
    }
    threadPool_.reset();
    ingestionPool_.reset();
}

// ---------------------------------------------------------------------------
// IWriter lifecycle
// ---------------------------------------------------------------------------

void MLDPGrpcWriter::start()
{
    if (running_.load())
    {
        warnf(*logger_, "MLDPGrpcWriter already started");
        return;
    }

    running_.store(true);
    infof(*logger_, "MLDPGrpcWriter starting");

    threadPool_ = std::make_shared<BS::light_thread_pool>(
        static_cast<std::size_t>(
            std::max(1, config_.threadPoolSize)));

    ingestionPool_ = MLDPGrpcIngestionePool::create(config_.poolConfig, metrics_);
    providerId_ = ingestionPool_->providerId();
    if (providerId_.empty())
    {
        running_.store(false);
        throw std::runtime_error("MLDPGrpcWriter: failed to register provider with MLDP ingestion service");
    }

    const std::size_t workerCount = std::max<std::size_t>(
        1, static_cast<std::size_t>(config_.threadPoolSize));
    nextChannel_.store(0, std::memory_order_relaxed);
    queuedItems_.store(0);
    channels_.clear();
    channels_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
    {
        channels_.push_back(std::make_unique<WorkerChannel>());
    }
    for (std::size_t i = 0; i < workerCount; ++i)
    {
        threadPool_->detach_task([this, i]() { workerLoop(i); });
    }

    infof(*logger_, "MLDPGrpcWriter started with {} workers", workerCount);
}

void MLDPGrpcWriter::stop() noexcept
{
    if (!running_.load())
    {
        return;
    }
    infof(*logger_, "MLDPGrpcWriter stopping");
    running_.store(false);
    for (auto& ch : channels_)
    {
        {
            std::lock_guard lk(ch->mutex);
            ch->shutdown = true;
        }
        ch->cv.notify_one();
    }
    if (threadPool_)
    {
        threadPool_->wait();
    }
    channels_.clear();
    infof(*logger_, "MLDPGrpcWriter stopped");
}

bool MLDPGrpcWriter::isHealthy() const noexcept
{
    return running_.load();
}

const std::string& MLDPGrpcWriter::providerId() const
{
    return providerId_;
}

// ---------------------------------------------------------------------------
// push — identical round-robin logic from the original controller
// ---------------------------------------------------------------------------

bool MLDPGrpcWriter::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (!running_.load())
    {
        return false;
    }
    if (batch.root_source.empty() || batch.frames.empty())
    {
        return false;
    }

    auto tags = std::make_shared<const std::vector<std::string>>(batch.tags);
    bool enqueued = false;
    for (auto& frame : batch.frames)
    {
        if (!hasTimestampListW(frame))
        {
            metric_call(metrics_, [&](auto& m)
                        { m.incrementBusFailures(1.0, {{"source", batch.root_source}}); });
            continue;
        }
        const auto idx = nextChannel_.fetch_add(1, std::memory_order_relaxed) % channels_.size();
        QueueItem  item{batch.root_source, tags, std::move(frame)};
        {
            std::lock_guard lk(channels_[idx]->mutex);
            channels_[idx]->items.push_back(std::move(item));
        }
        channels_[idx]->cv.notify_one();
        queuedItems_.fetch_add(1, std::memory_order_relaxed);
        enqueued = true;
    }
    updateQueueDepthMetric();
    return enqueued;
}

// ---------------------------------------------------------------------------
// workerLoop — identical to original MLDPPVXSController::workerLoop
// ---------------------------------------------------------------------------

void MLDPGrpcWriter::workerLoop(std::size_t workerIndex)
{
    auto& ch = *channels_[workerIndex];
    std::optional<mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject>> handle;
    std::unique_ptr<grpc::ClientWriter<dp::service::ingestion::IngestDataRequest>> writer;
    std::unique_ptr<grpc::ClientContext>                                            context;
    dp::service::ingestion::IngestDataStreamResponse                                response;
    std::chrono::steady_clock::time_point                                           streamStart;
    std::size_t                                                                     streamPayloadBytes = 0;
    std::uint64_t                                                                   requestCounter = 0;

    const auto close_stream = [&](const char* reason)
    {
        if (!writer) { return; }
        writer->WritesDone();
        auto    status = writer->Finish();
        int64_t requestedRequests = static_cast<int64_t>(requestCounter);
        if (status.ok())
        {
            if (response.has_ingestdatastreamresult())
            {
                const auto& result = response.ingestdatastreamresult();
                if (result.numrequests() < 0)
                {
                    errorf(*logger_, "Ingestion stream finished with invalid numrequests ({}): {}", reason, result.numrequests());
                }
                else if (result.numrequests() < requestedRequests)
                {
                    errorf(*logger_, "Ingestion stream finished with incomplete requests ({}): server accepted {} of {} sent",
                           reason, result.numrequests(), requestedRequests);
                }
                else if (result.numrequests() > requestedRequests)
                {
                    errorf(*logger_, "Ingestion stream finished with mismatch ({}): server reports {} but we sent {}",
                           reason, result.numrequests(), requestedRequests);
                }
                else
                {
                    tracef(*logger_, "Ingestion stream finished successfully ({}): {} requests", reason, result.numrequests());
                }
            }
            if (response.has_exceptionalresult())
            {
                errorf(*logger_, "Ingestion stream finished with exceptional result ({}): {}",
                       reason, response.exceptionalresult().message());
                metric_call(metrics_, [&](auto& m)
                            { m.incrementBusFailures(1.0, {{"source", "unknown"}}); });
            }
        }
        else
        {
            errorf(*logger_, "Ingestion stream finished with error ({}): {}", reason, status.error_message());
            metric_call(metrics_, [&](auto& m)
                        { m.incrementBusFailures(1.0, {{"source", "unknown"}}); });
        }
        writer.reset();
        handle.reset();
        context.reset();
        streamPayloadBytes = 0;
        requestCounter = 0;
    };

    const auto ensure_stream = [&]() -> bool
    {
        if (writer) { return true; }
        try
        {
            context = std::make_unique<grpc::ClientContext>();
            response = dp::service::ingestion::IngestDataStreamResponse();
            handle.emplace(ingestionPool_->acquire());
            writer = (*handle)->stub->ingestDataStream(context.get(), &response);
            if (!writer)
            {
                errorf(*logger_, "Failed to open ingestion stream");
                metric_call(metrics_, [&](auto& m)
                            { m.incrementBusFailures(1.0, {{"source", "unknown"}}); });
                handle.reset();
                context.reset();
                return false;
            }
            streamStart = std::chrono::steady_clock::now();
            streamPayloadBytes = 0;
            return true;
        }
        catch (const std::exception& ex)
        {
            errorf(*logger_, "Failed to acquire ingestion stream: {}", ex.what());
            metric_call(metrics_, [&](auto& m)
                        { m.incrementBusFailures(1.0, {{"source", "unknown"}}); });
            handle.reset();
            writer.reset();
            return false;
        }
    };

    const auto dequeueTimeout = config_.streamMaxAge;
    while (true)
    {
        QueueItem item;
        bool      hasItem = false;
        {
            std::unique_lock lk(ch.mutex);
            ch.cv.wait_for(lk, dequeueTimeout, [&]
                           { return !ch.items.empty() || ch.shutdown; });
            if (ch.shutdown && ch.items.empty()) { break; }
            if (!ch.items.empty())
            {
                item = std::move(ch.items.front());
                ch.items.pop_front();
                hasItem = true;
            }
        }

        if (!hasItem)
        {
            if (writer)
            {
                const auto elapsed = std::chrono::steady_clock::now() - streamStart;
                if (elapsed >= config_.streamMaxAge)
                {
                    close_stream("stream age exceeded (idle)");
                }
            }
            continue;
        }

        queuedItems_.fetch_sub(1, std::memory_order_relaxed);
        updateQueueDepthMetric();

        const auto itemStart = std::chrono::steady_clock::now();
        const auto record_send_time = [this, itemStart](prometheus::Labels tags)
        {
            const auto   elapsed = std::chrono::steady_clock::now() - itemStart;
            const double sec = std::chrono::duration<double>(elapsed).count();
            metric_call(metrics_, [&](auto& m)
                        { m.observeControllerSendTimeSeconds(sec, std::move(tags)); });
        };

        if (!ensure_stream())
        {
            record_send_time({{"source", "unknown"}});
            continue;
        }

        if (std::chrono::steady_clock::now() - streamStart >= config_.streamMaxAge)
        {
            close_stream("stream age exceeded");
            if (!ensure_stream())
            {
                record_send_time({{"source", "unknown"}});
                continue;
            }
        }

        google::protobuf::Arena arena;
        auto* request = google::protobuf::Arena::CreateMessage<
            dp::service::ingestion::IngestDataRequest>(&arena);
        std::size_t acceptedEvents = 0;
        std::size_t payloadBytes = 0;
        const auto  requestId = mldp_pvxs_driver::util::format_string(
            "pv_stream_{}_{}_{}", streamStart.time_since_epoch().count(),
            item.root_source, requestCounter++);

        if (!buildRequest(item.root_source, item.frame, requestId,
                          *request, acceptedEvents, payloadBytes))
        {
            continue;
        }

        if ((streamPayloadBytes + payloadBytes) > config_.streamMaxBytes &&
            streamPayloadBytes > 0)
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
            errorf(*logger_, "Failed to write source {} to ingestion stream", item.root_source);
            metric_call(metrics_, [&](auto& m)
                        { m.incrementBusFailures(1.0, {{"source", item.root_source}}); });
            close_stream("write failed");
            continue;
        }

        streamPayloadBytes += payloadBytes;
        if (acceptedEvents > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        { m.incrementBusPushes(static_cast<double>(acceptedEvents),
                                               {{"source", item.root_source}}); });
        }
        if (payloadBytes > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        { m.incrementBusPayloadBytes(static_cast<double>(payloadBytes),
                                                     {{"source", item.root_source}}); });
            const auto   elapsed = std::chrono::steady_clock::now() - itemStart;
            const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
            if (ms > 0.0)
            {
                const double bps = (static_cast<double>(payloadBytes) * 1000.0) / ms;
                metric_call(metrics_, [&](auto& m)
                            { m.setBusPayloadBytesPerSecond(bps, {{"source", item.root_source}}); });
            }
        }
        record_send_time({{"source", item.root_source}});

        const auto postElapsed = std::chrono::steady_clock::now() - streamStart;
        if (postElapsed >= config_.streamMaxAge ||
            streamPayloadBytes >= config_.streamMaxBytes)
        {
            close_stream("threshold reached");
        }
    }

    close_stream("shutdown");
}

// ---------------------------------------------------------------------------
// buildRequest — identical to original MLDPPVXSController::buildRequest
// ---------------------------------------------------------------------------

bool MLDPGrpcWriter::buildRequest(const std::string&                         sourceName,
                                   const dp::service::common::DataFrame&      frame,
                                   const std::string&                         requestId,
                                   dp::service::ingestion::IngestDataRequest& request,
                                   std::size_t&                               acceptedEvents,
                                   std::size_t&                               payloadBytes)
{
    request.set_providerid(providerId_);
    request.set_clientrequestid(requestId);

    auto* dataFrame = request.mutable_ingestiondataframe();
    *dataFrame = frame;

    const bool hasColumns =
        dataFrame->doublecolumns_size() > 0 || dataFrame->floatcolumns_size() > 0 ||
        dataFrame->datacolumns_size() > 0   || dataFrame->int32columns_size() > 0 ||
        dataFrame->int64columns_size() > 0  || dataFrame->boolcolumns_size() > 0  ||
        dataFrame->stringcolumns_size() > 0 || dataFrame->enumcolumns_size() > 0  ||
        dataFrame->imagecolumns_size() > 0  || dataFrame->structcolumns_size() > 0 ||
        dataFrame->doublearraycolumns_size() > 0 || dataFrame->floatarraycolumns_size() > 0 ||
        dataFrame->int32arraycolumns_size() > 0  || dataFrame->int64arraycolumns_size() > 0 ||
        dataFrame->boolarraycolumns_size() > 0;

    if (!hasColumns)
    {
        warnf(*logger_, "No valid columns for source {}, skipping request", sourceName);
        return false;
    }

    if (!hasTimestampListW(*dataFrame))
    {
        errorf(*logger_, "Dropping frame for source {}: missing DataFrame.datatimestamps.timestamplist", sourceName);
        metric_call(metrics_, [&](auto& m)
                    { m.incrementBusFailures(1.0, {{"source", sourceName}}); });
        return false;
    }

    acceptedEvents = static_cast<std::size_t>(
        dataFrame->datatimestamps().timestamplist().timestamps_size());
    payloadBytes = static_cast<std::size_t>(request.ByteSizeLong());
    return true;
}

// ---------------------------------------------------------------------------
// Metrics helper
// ---------------------------------------------------------------------------

void MLDPGrpcWriter::updateQueueDepthMetric()
{
    const double depth = static_cast<double>(queuedItems_.load(std::memory_order_relaxed));
    metric_call(metrics_, [&](auto& m) { m.setControllerQueueDepth(depth); });
}
