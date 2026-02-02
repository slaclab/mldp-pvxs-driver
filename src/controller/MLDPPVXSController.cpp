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

#include <chrono>
#include <format>
#include <grpcpp/grpcpp.h>
#include <ranges>
#include <stdexcept>

using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader;
using namespace mldp_pvxs_driver::util::log;

using mldp_pvxs_driver::util::pool::MLDPGrpcPool;

namespace {
std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeControllerLogger()
{
    std::string loggerName = "controlller";
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
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
    mldp_pool_.reset();
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

    running_.store(true);
    infof(*logger_, "Controller is starting");
    // Start allocating mldp pool (constructor registers provider)
    mldp_pool_ = MLDPGrpcPool::create(config_.pool(), metrics_);
    provider_id_ = mldp_pool_->providerId();
    if (provider_id_.empty())
    {
        running_ = false;
        throw std::runtime_error("Failed to register provider with MLDP ingestion service");
    }

    const std::size_t worker_count = static_cast<std::size_t>(config_.controllerThreadPoolSize());
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
    if (provider_id_.empty())
    {
        errorf(*logger_, "Provider not registered; dropping event batch");
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementBusFailures(1.0, {{"reader", "unknown"}});
                    });
        return false;
    }

    const bool hasEvents = std::ranges::any_of(batch_values.values,
                                               [](const auto& entry)
                                               {
                                                   return !entry.second.empty();
                                               });
    if (!hasEvents)
    {
        warnf(*logger_, "Received empty batch for ingestion, skipping push.");
        return false;
    }

    auto tags = std::make_shared<const std::vector<std::string>>(batch_values.tags);
    for (auto& [src_name, events] : batch_values.values)
    {
        if (events.empty())
        {
            continue;
        }

        QueueItem item{
            batch_values.root_source,
            tags,
            src_name,
            std::move(events)};

        auto idx = std::hash<std::string>{}(src_name) % channels_.size();
        {
            std::lock_guard lk(channels_[idx]->mutex);
            channels_[idx]->items.push_back(std::move(item));
        }
        channels_[idx]->cv.notify_one();
        queued_items_.fetch_add(1, std::memory_order_relaxed);
    }

    updateQueueDepthMetric();
    return true;
}

void MLDPPVXSController::workerLoop(std::size_t worker_index)
{
    // Worker threads block on the shared queue, build per-source ingestion
    // requests, and write them into a gRPC client-streaming RPC. Each worker
    // keeps a single stream open at a time and flushes (closes/reopens) when
    // byte or age thresholds are reached or when a write fails.
    auto& ch = *channels_[worker_index];
    std::optional<util::pool::PooledHandle<util::pool::MLDPGrpcObject>> handle;
    std::unique_ptr<grpc::ClientWriter<dp::service::ingestion::IngestDataRequest>> writer;
    std::unique_ptr<grpc::ClientContext> context;
    dp::service::ingestion::IngestDataStreamResponse response;
    std::chrono::steady_clock::time_point stream_start;
    std::size_t stream_payload_bytes = 0;

    const auto close_stream = [&](const char* reason)
    {
        if (!writer)
        {
            return;
        }
        writer->WritesDone();
        const auto status = writer->Finish();
        if (!status.ok())
        {
            errorf(*logger_, "Ingestion stream failed ({}) : {}", reason, status.error_message());
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
            handle.emplace(mldp_pool_->acquire());
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

    const auto dequeue_timeout = config_.controllerStreamMaxAge();
    while (true)
    {
        QueueItem item;
        bool has_item = false;
        {
            std::unique_lock lk(ch.mutex);
            ch.cv.wait_for(lk, dequeue_timeout, [&] { return !ch.items.empty() || ch.shutdown; });
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

        google::protobuf::Arena arena;
        auto* request = google::protobuf::Arena::CreateMessage<dp::service::ingestion::IngestDataRequest>(&arena);
        std::size_t accepted_events = 0;
        std::size_t payload_bytes = 0;
        const auto request_id = std::format("pv_stream_{}_{}", stream_start.time_since_epoch().count(), item.src_name);
        if (!buildRequest(item, request_id, *request, accepted_events, payload_bytes))
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
            errorf(*logger_, "Failed to write data column {} with {} events to ingestion stream", item.src_name, accepted_events);
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0, {{"source", item.root_source}});
                        });
            close_stream("write failed");
            record_send_time({{"source", item.root_source}});
            continue;
        }

        stream_payload_bytes += payload_bytes;
        if (accepted_events > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusPushes(static_cast<double>(accepted_events), {{"source", item.root_source}});
                        });
        }
        if (payload_bytes > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusPayloadBytes(static_cast<double>(payload_bytes), {{"source", item.root_source}});
                        });
            const auto   item_elapsed = std::chrono::steady_clock::now() - item_start;
            const double elapsed_milliseconds = std::chrono::duration<double, std::milli>(item_elapsed).count();
            if (elapsed_milliseconds > 0.0)
            {
                const double bytes_per_second = (static_cast<double>(payload_bytes) * 1000.0) / elapsed_milliseconds;
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

bool MLDPPVXSController::buildRequest(const QueueItem& item,
                                      const std::string& request_id,
                                      dp::service::ingestion::IngestDataRequest& request,
                                      std::size_t& accepted_events,
                                      std::size_t& payload_bytes)
{
    request.set_providerid(provider_id_);
    request.set_clientrequestid(request_id);

    if (item.tags)
    {
        for (const auto& tag : *item.tags)
        {
            request.add_tags(tag);
        }
    }

    auto* dataFrame = request.mutable_ingestiondataframe();
    auto* timestamps = dataFrame->mutable_datatimestamps();
    auto* timestampList = timestamps->mutable_timestamplist();
    auto* column = dataFrame->add_datacolumns();
    column->set_name(item.src_name);

    const int eventCount = static_cast<int>(item.events.size());
    timestampList->mutable_timestamps()->Reserve(eventCount);
    column->mutable_datavalues()->Reserve(eventCount);

    for (auto& event_value : item.events)
    {
        if (!event_value)
        {
            warnf(*logger_, "Skipping null event for source {}", item.src_name);
            continue;
        }

        auto* ts = timestampList->add_timestamps();
        if (event_value->epoch_seconds)
        {
            ts->set_epochseconds(event_value->epoch_seconds);
            if (event_value->nanoseconds)
            {
                ts->set_nanoseconds(event_value->nanoseconds);
            }
        }
        else
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            ts->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(now).count());
        }

        auto* dataValue = column->add_datavalues();
        *dataValue = std::move(event_value->data_value);
        ++accepted_events;
    }

    if (column->datavalues_size() == 0)
    {
        warnf(*logger_, "No valid events for source {}, skipping request", item.src_name);
        return false;
    }

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
