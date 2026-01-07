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

#include <chrono>
#include <format>
#include <grpcpp/grpcpp.h>
#include <ranges>
#include <stdexcept>
#include <unordered_map>

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
    if (running_)
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
    if (running_)
    {
        warnf(*logger_, "Controller is already started");
        return;
    }

    running_ = true;
    infof(*logger_, "Controller is starting");
    // Start allocating mldp pool (constructor registers provider)
    mldp_pool_ = MLDPGrpcPool::create(config_.pool(), metrics_);
    provider_id_ = mldp_pool_->providerId();
    if (provider_id_.empty())
    {
        running_ = false;
        throw std::runtime_error("Failed to register provider with MLDP ingestion service");
    }

    // Start readers (dispatch based on declared reader type)
    infof(*logger_, "Starting readers");
    for (const auto& entry : config_.readerEntries())
    {
        const auto& type = entry.first;
        const auto& readerConfig = entry.second;
        auto        reader = ReaderFactory::create(type, shared_from_this(), readerConfig);
        readers_.push_back(std::move(reader));
    }
    infof(*logger_, "Controller started");
}

void MLDPPVXSController::stop()
{
    if (running_ == false)
    {
        warnf(*logger_, "Controller already stopped");
        return;
    }
    infof(*logger_, "Controller is stopping");
    running_ = false;
    // clear readers
    readers_.clear();
    // Stop controller logic
    thread_pool_->wait();
    infof(*logger_, "Controller stopped");
}

bool MLDPPVXSController::push(EventBatch batch_values)
{
    if (!running_)
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

    thread_pool_->detach_task([this, batch_values = std::move(batch_values)]() mutable
                              {
                                  // Execute the push in the thread pool
                                  pushImpl(std::move(batch_values));
                              });
    return true;
}

void MLDPPVXSController::pushImpl(EventBatch batch_values)
{
    try
    {
        const auto start_time = std::chrono::steady_clock::now();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());

        grpc::ClientContext                              context;
        dp::service::ingestion::IngestDataStreamResponse response;
        auto                                             pool_instance = mldp_pool_->acquire();
        auto                                             writer = pool_instance->stub->ingestDataStream(&context, &response);

        if (!writer)
        {
            errorf(*logger_, "Failed to open ingestion stream for incoming batch");
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0, {{"reader", "unknown"}});
                        });
            return;
        }

        const auto base_request_id = std::format("pv_batch_{}", now_ms.count());
        size_t     accepted_events = 0;
        bool       wrote_any = false;
        uint64_t   total_payload_bytes = 0;
        std::unordered_map<std::string, uint64_t> payload_bytes_by_reader;

        for (auto& [src_name, events] : batch_values.values)
        {;
            if (events.empty())
            {
                continue;
            }

            dp::service::ingestion::IngestDataRequest request;
            request.set_providerid(provider_id_);
            request.set_clientrequestid(std::format("{}_{}", base_request_id, src_name));

            if (!batch_values.tags.empty())
            {
                for (const auto& tag : batch_values.tags)
                {
                    request.add_tags(tag);
                }
            }

            auto* dataFrame = request.mutable_ingestiondataframe();
            auto* timestamps = dataFrame->mutable_datatimestamps();
            auto* timestampList = timestamps->mutable_timestamplist();
            auto* column = dataFrame->add_datacolumns();
            column->set_name(src_name);

            for (auto& event_value : events)
            {
                if (!event_value)
                {
                    warnf(*logger_, "Skipping null event for source {}", src_name);
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

                if (event_value->data_value)
                {
                    auto* dataValue = column->add_datavalues();
                    *dataValue = std::move(*event_value->data_value);
                    ++accepted_events;
                }
                else
                {
                    warnf(*logger_, "Missing data_value content for source {}", src_name);
                }
            }

            if (column->datavalues_size() == 0)
            {
                continue;
            }

            const auto payload_bytes = request.ByteSizeLong();

            if (!writer->Write(request))
            {
                errorf(*logger_, "Failed to write data column {} with {} events to ingestion stream", src_name, column->datavalues_size());
                writer->WritesDone();
                writer->Finish();
                metric_call(metrics_, [&](auto& m)
                            {
                                m.incrementBusFailures(1.0, {{"reader", src_name}});
                            });
                return;
            }

            total_payload_bytes += payload_bytes;
            payload_bytes_by_reader[src_name] += payload_bytes;
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusPayloadBytes(static_cast<double>(payload_bytes), {{"reader", src_name}});
                            m.incrementBusPushes(static_cast<double>(column->datavalues_size()), {{"reader", src_name}});
                        });

            wrote_any = true;
        }

        if (!wrote_any)
        {
            warn("No valid events in batch, skipping push.");
            writer->WritesDone();
            writer->Finish();
            return;
        }

        writer->WritesDone();
        const auto status = writer->Finish();
        if (status.ok())
        {
            // Update bytes/second metric
            const auto elapsed = std::chrono::steady_clock::now() - start_time;
            const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
            if (elapsed_seconds > 0.0)
            {
                for (const auto& [reader, bytes] : payload_bytes_by_reader)
                {
                    const double bytes_per_second = static_cast<double>(bytes) / elapsed_seconds;
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.setBusPayloadBytesPerSecond(bytes_per_second, {{"reader", reader}});
                                });
                }
            }
        }
        else
        {
            // Update metrics for push failure
            errorf(*logger_, "Ingestion stream failed for batch of {} events: {}", accepted_events, status.error_message());
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementBusFailures(1.0);
                        });
        }
    }
    catch (const std::exception& ex)
    {
        // Update metrics for readers errors
        errorf(*logger_, "Failed to push event batch: {}", ex.what());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0);
                    });
    }
}

Metrics& MLDPPVXSController::metrics() const
{
    if (!metrics_)
    {
        throw std::runtime_error("Metrics not configured for controller");
    }
    return *metrics_;
}
