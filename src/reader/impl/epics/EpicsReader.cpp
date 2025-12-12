
#include "util/bus/IEventBusPush.h"
#include <chrono>
#include <cstdint>
#include <metrics/Metrics.h>
#include <reader/impl/epics/BSASEpicsMLDPConversion.h>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <reader/impl/epics/EpicsReader.h>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <utility>

using namespace pvxs::client;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::reader::impl::epics;

using MldpDriverConfig = mldp_pvxs_driver::config::Config;

EpicsReader::EpicsReader(std::shared_ptr<IEventBusPush>                      bus,
                         std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics,
                         const MldpDriverConfig&                             cfg)
    : Reader(std::move(bus), std::move(metrics))
    , config_(EpicsReaderConfig(cfg))
    , name_(config_.name())
    , running_(false)
{

    running_ = true;
    pva_context_ = pvxs::client::Context::fromEnv();
    worker_ = std::thread([this]
                          {
                              run(-1);
                          });
    // add all configured pvs
    PVSet pvNames;
    for (const auto& pvConfig : config_.pvs())
    {
        pvNames.insert(pvConfig.name);

        PVRuntimeConfig runtime;
        if (pvConfig.nttableRowTs.has_value())
        {
            runtime.mode = PVRuntimeConfig::Mode::NtTableRowTs;
            runtime.tsSecondsField = pvConfig.nttableRowTs->tsSecondsField;
            runtime.tsNanosField = pvConfig.nttableRowTs->tsNanosField;
        }
        pvRuntimeByName_.emplace(pvConfig.name, std::move(runtime));
    }
    addPV(pvNames);
}

EpicsReader::~EpicsReader()
{
    running_ = false;
    m_pva_workqueue.push(nullptr); // Push a null subscription to unblock the queue
    if (worker_.joinable())
        worker_.join();
}

std::string EpicsReader::name() const
{
    return name_;
}

void EpicsReader::addPV(const PVSet& pvNames)
{
    for (const auto& pv : pvNames)
    {
        auto pv_mon = pva_context_.monitor(pv)
                          .event([this](const pvxs::client::Subscription& s)
                                 {
                                     m_pva_workqueue.push(s.shared_from_this());
                                 })
                          .exec();
        m_pva_subscriptions.push(pv_mon);
        spdlog::info("Started monitoring PV {} on reader {}", pv, name_);
    }
}

void EpicsReader::run(int timeout)
{
    spdlog::info("EpicsReader worker thread started on reader {}.", name_);
    bool       expired = false;
    const auto acquisition_ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    while (running_ && !expired)
    {
        auto sub = m_pva_workqueue.pop();
        if (!sub)
            continue; // Skip null subscription used to unblock queue on shutdown
        const prometheus::Labels readerTags{{"reader", name_}};
        try
        {
            pvxs::Value epics_value = sub->pop();
            if (!epics_value)
            {
                // no data available
                continue;
            }

            // get the PV name and runtime configuration
            const auto                  pvName = sub->name();
            const auto                  it = pvRuntimeByName_.find(pvName);
            const PVRuntimeConfig::Mode mode = (it != pvRuntimeByName_.end()) ? it->second.mode : PVRuntimeConfig::Mode::Default;

            // prepare batch
            IEventBusPush::EventBatch batch;
            // keep track of how many events were emitted
            size_t                    emitted = 0;

            switch (mode)
            {
            case PVRuntimeConfig::Mode::NtTableRowTs:
                {
                    if (BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(
                            pvName, epics_value, it->second.tsSecondsField, it->second.tsNanosField, &batch, &emitted))
                    {
                        // batch + emitted were filled
                        spdlog::error("Converted PV {} to MLDP NtTableRowTs batch on reader {}.", pvName, name_);
                        MLDP_METRICS_CALL(metrics_, incrementReaderEvents(static_cast<double>(emitted), readerTags));
                    }
                }
                break;

            case PVRuntimeConfig::Mode::Default:
            default:
                {
                    uint64_t epoch_seconds = 0;
                    uint64_t nanoseconds = 0;
                    if (epics_value.type().kind() == pvxs::Kind::Compound)
                    {
                        bool setEpoch = false;
                        if (const auto timestampField = epics_value["timeStamp"]; timestampField.valid())
                        {
                            if (const auto secondsField = timestampField["secondsPastEpoch"]; secondsField.valid())
                            {
                                epoch_seconds = secondsField.as<uint64_t>();
                                setEpoch = true;
                            }
                            if (const auto nanosecondsField = timestampField["nanoseconds"]; nanosecondsField.valid())
                            {
                                nanoseconds = nanosecondsField.as<uint64_t>();
                            }
                        }
                        if (!setEpoch)
                        {
                            // Fallback to make sure timestamp is always set
                            epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(acquisition_ts).count();
                        }
                    }

                    // allocate event value
                    auto event_value = IEventBusPush::MakeEventValue(epoch_seconds, nanoseconds);

                    // convert PVXS value to MLDP proto value
                    EpicsMLDPConversion::convertPVToProtoValue(epics_value, event_value->data_value.get());

                    // push to bus moving data and batching per source
                    batch.tags.push_back(pvName);
                    batch.values[pvName].emplace_back(std::move(event_value));
                    emitted = 1;
                }
                break;
            }

            if (emitted > 0 && !batch.values.empty())
            {
                bus_->push(std::move(batch));
                MLDP_METRICS_CALL(metrics_, incrementReaderEvents(static_cast<double>(emitted), readerTags));
            }
        }
        catch (const pvxs::client::RemoteError& e)
        {
            spdlog::error("Server error when reading PV {} on reader {}: {}", sub->name(), name_, e.what());
            MLDP_METRICS_CALL(metrics_, incrementReaderErrors(1.0, readerTags));
        }

        if (timeout > 0)
        {
            if (
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()) - acquisition_ts;
                elapsed.count() > timeout)
            {
                expired = true;
            }
        }
    }
    spdlog::info("EpicsReader worker thread exiting on reader {}.", name_);
}
