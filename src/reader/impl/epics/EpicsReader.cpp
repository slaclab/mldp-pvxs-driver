//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <metrics/Metrics.h>
#include <reader/impl/epics/BSASEpicsMLDPConversion.h>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <reader/impl/epics/EpicsReader.h>
#include <string_view>
#include <util/bus/IEventBusPush.h>
#include <util/log/Logger.h>
#include <utility>

#include <chrono>
#include <cstdint>
#include <unordered_map>

using namespace pvxs::client;

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;

using MldpDriverConfig = mldp_pvxs_driver::config::Config;

namespace {
std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeEpicsReaderLogger(const std::string& readerName)
{
    std::string loggerName = "epics_reader";
    if (!readerName.empty())
    {
        loggerName += ":";
        loggerName += readerName;
    }
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}
} // namespace

EpicsReader::EpicsReader(std::shared_ptr<IEventBusPush> bus,
                         std::shared_ptr<Metrics>       metrics,
                         const MldpDriverConfig&        cfg)
    : Reader(std::move(bus), std::move(metrics))
    , logger_(makeEpicsReaderLogger(cfg.get("name")))
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
    m_pva_workqueue.push(ProcessingValue{}); // Push a null subscription to unblock the queue
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
                          .event([this](pvxs::client::Subscription& s)
                                 {
                                     // drain all available values
                                     for (pvxs::Value value = s.pop(); value; value = s.pop())
                                     {
                                         m_pva_workqueue.push(ProcessingValue{s.name(), std::move(value)});
                                     }
                                 })
                          .exec();
        m_pva_subscriptions.push(pv_mon);
        infof(*logger_, "[{}/{}] Started monitoring", name_, pv);
    }
}

void EpicsReader::run(int timeout)
{
    infof(*logger_, "EpicsReader worker thread started on reader {}.", name_);
    bool       expired = false;
    const auto acquisition_ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    while (running_ && !expired)
    {
        auto                     processing_value = m_pva_workqueue.pop();
        std::string              pvName = std::move(processing_value.pvName);
        pvxs::Value              epics_value = std::move(processing_value.value);
        const prometheus::Labels sourceTag{{"source", pvName}};
        try
        {
            // Time here is get to calculate the rpocessing time later
            const auto processing_start = std::chrono::steady_clock::now();

            // get the PV name and runtime configuration
            const auto                  it = pvRuntimeByName_.find(pvName);
            const PVRuntimeConfig::Mode mode = (it != pvRuntimeByName_.end()) ? it->second.mode : PVRuntimeConfig::Mode::Default;

            // prepare batch
            IEventBusPush::EventBatch batch;
            // keep track of how many events were emitted
            size_t emitted = 0;

            switch (mode)
            {
            case PVRuntimeConfig::Mode::NtTableRowTs:
                {
                    if (!BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(
                            *logger_, pvName, epics_value, it->second.tsSecondsField, it->second.tsNanosField, &batch, emitted))
                    {
                        // batch + emitted were filled
                        errorf(*logger_, "Error converting PV {} to MLDP NtTableRowTs batch on reader {}.", pvName, name_);
                        metric_call(metrics_, [&](auto& m)
                                    {
                                        m.incrementReaderErrors(1.0, sourceTag);
                                    });
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

                    // Convert only the nested EPICS 'value' field (NTScalar/NTScalarArray/NTTable/etc)
                    // so we don't serialize metadata like alarm/timeStamp into MLDP.
                    const pvxs::Value valueField = (epics_value.type().kind() == pvxs::Kind::Compound) ? epics_value["value"] : pvxs::Value{};
                    EpicsMLDPConversion::convertPVToProtoValue(valueField.valid() ? valueField : epics_value, event_value->data_value.get());

                    const pvxs::Value alarm = epics_value["alarm"];
                    if (alarm.valid())
                    {
                        auto* valueStatus = event_value->data_value->mutable_valuestatus();

                        if (const auto severityField = alarm["severity"]; severityField.valid())
                        {
                            const int sev = severityField.as<int>();
                            switch (sev)
                            {
                            case 0:
                                valueStatus->set_severity(DataValue_ValueStatus_Severity_NO_ALARM);
                                break;
                            case 1:
                                valueStatus->set_severity(DataValue_ValueStatus_Severity_MINOR_ALARM);
                                break;
                            case 2:
                                valueStatus->set_severity(DataValue_ValueStatus_Severity_MAJOR_ALARM);
                                break;
                            case 3:
                                valueStatus->set_severity(DataValue_ValueStatus_Severity_INVALID_ALARM);
                                break;
                            default:
                                valueStatus->set_severity(DataValue_ValueStatus_Severity_UNDEFINED_ALARM);
                                break;
                            }
                        }

                        if (const auto statusField = alarm["status"]; statusField.valid())
                        {
                            // EPICS alarm status is a numeric code that doesn't map 1:1 onto
                            // DataValue.ValueStatus.StatusCode; use RECORD_STATUS to indicate
                            // the record/PV reported a status condition.
                            const int status = statusField.as<int>();
                            valueStatus->set_statuscode(status == 0 ? DataValue_ValueStatus_StatusCode_NO_STATUS
                                                                    : DataValue_ValueStatus_StatusCode_RECORD_STATUS);
                        }

                        if (const auto messageField = alarm["message"]; messageField.valid())
                        {
                            valueStatus->set_message(messageField.as<std::string>());
                        }
                    }

                    // push to bus moving data and batching per source
                    batch.tags.push_back(pvName);
                    batch.values[pvName].emplace_back(std::move(event_value));
                    emitted = 1;
                }
                break;
            }

            const auto   processing_end = std::chrono::steady_clock::now();
            const double processing_seconds = std::chrono::duration<double>(processing_end - processing_start).count();
            metric_call(metrics_, [&](auto& m)
                        {
                            m.observeReaderProcessingTimeSeconds(processing_seconds, sourceTag);
                        });

            if (!batch.values.empty())
            {
                bus_->push(std::move(batch));
                metric_call(metrics_, [&](auto& m)
                            {
                                m.incrementReaderEvents(static_cast<double>(1.0), sourceTag);
                            });
                tracef(*logger_, "[{}/{}] event published", name_, pvName);
            }
        }
        catch (const pvxs::client::RemoteError& e)
        {
            errorf(*logger_, "Server error when reading PV {} on reader {}: {}", pvName, name_, e.what());
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementReaderErrors(1.0, sourceTag);
                        });
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
    infof(*logger_, "EpicsReader worker thread exiting on reader {}.", name_);
}
