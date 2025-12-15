#include <controller/MLDPPVXSController.h>
#include <memory>
#include <reader/ReaderFactory.h>
#include <util/log/Logger.h>

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

std::shared_ptr<MLDPPVXSController> MLDPPVXSController::create(const config::Config& config)
{
    return std::shared_ptr<MLDPPVXSController>(new MLDPPVXSController(config));
}

MLDPPVXSController::MLDPPVXSController(const config::Config& config)
    : config_(config)
    , thread_pool_(std::make_shared<BS::light_thread_pool>(config_.controllerThreadPoolSize()))
    , metrics_(std::make_shared<metrics::Metrics>(*config_.metricsConfig()))
    , running_(false)
{
    // Constructor implementation
}

MLDPPVXSController::~MLDPPVXSController()
{
    // Destructor implementation
    stop();
}

void MLDPPVXSController::start()
{
    if (running_)
    {
        return;
    }

    running_ = true;
    // Start allocating mldp pool (constructor registers provider)
    mldp_pool_ = MLDPGrpcPool::create(config_.pool(), metrics_);
    provider_id_ = mldp_pool_->providerId();
    if (provider_id_.empty())
    {
        running_ = false;
        throw std::runtime_error("Failed to register provider with MLDP ingestion service");
    }

    // Start readers (dispatch based on declared reader type)
    for (const auto& entry : config_.readerEntries())
    {
        const auto& type = entry.first;
        const auto& readerConfig = entry.second;
        auto        reader = ReaderFactory::create(type, shared_from_this(), readerConfig);
        readers_.push_back(std::move(reader));
    }
}

void MLDPPVXSController::stop()
{
    if (running_ == false)
    {
        return;
    }
    running_ = false;
    // clear readers
    readers_.clear();
    // Stop controller logic
    thread_pool_->wait();
}

bool MLDPPVXSController::push(EventBatch batch_values)
{
    if (!running_)
    {
        return false;
    }
    if (provider_id_.empty())
    {
        error("Provider not registered; dropping event batch");
        MLDP_METRICS_CALL(metrics_, incrementBusFailures());
        return false;
    }

    const bool hasEvents = std::ranges::any_of(batch_values.values,
                                               [](const auto& entry)
                                               {
                                                   return !entry.second.empty();
                                               });
    if (!hasEvents)
    {
        warn("Received empty batch for ingestion, skipping push.");
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
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());

        grpc::ClientContext                              context;
        dp::service::ingestion::IngestDataStreamResponse response;
        auto                                             pool_instance = mldp_pool_->acquire();
        auto                                             writer = pool_instance->stub->ingestDataStream(&context, &response);

        if (!writer)
        {
            error("Failed to open ingestion stream for incoming batch");
            MLDP_METRICS_CALL(metrics_, incrementBusFailures());
            return;
        }

        const auto base_request_id = std::format("pv_batch_{}", now_ms.count());
        size_t     accepted_events = 0;
        bool       wrote_any = false;

        for (auto& [src_name, events] : batch_values.values)
        {
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
                    warnf("Skipping null event for source {}", src_name);
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
                    warnf("Missing data_value content for source {}", src_name);
                }
            }

            if (column->datavalues_size() == 0)
            {
                continue;
            }

            if (!writer->Write(request))
            {
                errorf("Failed to write data column {} with {} events to ingestion stream", src_name, column->datavalues_size());
                writer->WritesDone();
                writer->Finish();
                MLDP_METRICS_CALL(metrics_, incrementBusFailures());
                return;
            }

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
            MLDP_METRICS_CALL(metrics_, incrementBusPushes());
        }
        else
        {
            errorf("Ingestion stream failed for batch of {} events: {}", accepted_events, status.error_message());
            MLDP_METRICS_CALL(metrics_, incrementBusFailures());
        }
    }
    catch (const std::exception& ex)
    {
        errorf("Failed to push event batch: {}", ex.what());
        MLDP_METRICS_CALL(metrics_, incrementReaderErrors());
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
