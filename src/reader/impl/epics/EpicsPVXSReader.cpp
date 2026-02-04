//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/EpicsPVXSReader.h>

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/impl/epics/BSASEpicsMLDPConversion.h>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <util/log/Logger.h>

#include <chrono>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::util::bus;

namespace {
std::shared_ptr<ILogger> makeLogger(const std::string& readerName)
{
    std::string loggerName = "epics_reader_pvxs";
    if (!readerName.empty())
    {
        loggerName += ":";
        loggerName += readerName;
    }
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}

constexpr const char* kPVXSDefaultMonitorRequest = "field(value,alarm,timeStamp)";
} // namespace

EpicsPVXSReader::EpicsPVXSReader(std::shared_ptr<util::bus::IEventBusPush> bus,
                                 std::shared_ptr<metrics::Metrics>         metrics,
                                 const config::Config&                     cfg)
    : EpicsReaderBase(std::move(bus), std::move(metrics), EpicsReaderConfig(cfg), makeLogger(cfg.get("name")))
{
    pva_context_ = pvxs::client::Context::fromEnv();
    addPV(pvNames());
}

void EpicsPVXSReader::addPV(const PVSet& pvNames)
{
    for (const auto& pv : pvNames)
    {
        auto        monitor = pva_context_.monitor(pv);
        const auto* runtimeCfg = runtimeConfigFor(pv);
        const auto  mode = runtimeCfg ? runtimeCfg->mode : PVRuntimeConfig::Mode::Default;

        auto pv_mon = monitor
                          .pvRequest(kPVXSDefaultMonitorRequest)
                          .event([this](pvxs::client::Subscription& s)
                                 {
                                     for (pvxs::Value value = s.pop(); value; value = s.pop())
                                     {
                                         metric_call(metrics_, [&](auto& m)
                                                     {
                                                         m.incrementReaderEventsReceived(1.0, {{"source", s.name()}});
                                                     });
                                         std::string pvName = s.name();
                                         reader_pool_->detach_task(
                                             [this, n = std::move(pvName), v = std::move(value)]() mutable
                                             {
                                                 processEvent(std::move(n), std::move(v));
                                             });
                                         metric_call(metrics_, [&](auto& m)
                                                     {
                                                         m.setReaderPoolQueueDepth(
                                                             static_cast<double>(reader_pool_->get_tasks_queued()),
                                                             {{"reader", name_}});
                                                     });
                                     }
                                 })
                          .exec();
        m_pva_subscriptions.push(pv_mon);
        infof(*logger_, "[{}/{}] Started monitoring", name_, pv);
    }
}

void EpicsPVXSReader::processEvent(std::string pvName, pvxs::Value epics_value)
{
    if (!running_.load())
    {
        return;
    }

    metric_call(metrics_, [&](auto& m)
                {
                    m.setReaderPoolQueueDepth(
                        static_cast<double>(reader_pool_->get_tasks_queued()),
                        {{"reader", name_}});
                });

    const prometheus::Labels sourceTag{{"source", pvName}};
    try
    {
        const auto processing_start = std::chrono::steady_clock::now();

        const auto* runtimeCfg = runtimeConfigFor(pvName);
        const auto  mode = runtimeCfg ? runtimeCfg->mode : PVRuntimeConfig::Mode::Default;

        size_t emitted = 0;

        switch (mode)
        {
        case PVRuntimeConfig::Mode::NtTableRowTs:
            {
                const std::size_t         colBatchSize = config_.columnBatchSize();
                IEventBusPush::EventBatch tableBatch;
                tableBatch.root_source = pvName;
                tableBatch.tags.push_back(pvName);
                std::size_t colsInBatch = 0;

                if (!BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(
                        *logger_, pvName, epics_value,
                        runtimeCfg ? runtimeCfg->tsSecondsField : "secondsPastEpoch",
                        runtimeCfg ? runtimeCfg->tsNanosField : "nanoseconds",
                        [&](std::string colName, std::vector<IEventBusPush::EventValue> events)
                        {
                            tableBatch.values[std::move(colName)] = std::move(events);
                            ++colsInBatch;
                            if (colBatchSize > 0 && colsInBatch >= colBatchSize)
                            {
                                bus_->push(std::move(tableBatch));
                                tableBatch = IEventBusPush::EventBatch{};
                                tableBatch.root_source = pvName;
                                tableBatch.tags.push_back(pvName);
                                colsInBatch = 0;
                            }
                        },
                        emitted,
                        reader_pool_->get_thread_count() > 1 ? reader_pool_.get() : nullptr))
                {
                    errorf(*logger_, "Error converting PV {} to MLDP NtTableRowTs batch on reader {}.", pvName, name_);
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.incrementReaderErrors(1.0, sourceTag);
                                });
                }
                else if (!tableBatch.values.empty())
                {
                    bus_->push(std::move(tableBatch));
                }
            }
            break;

        case PVRuntimeConfig::Mode::Default:
        default:
            {
                if (epics_value.type().kind() != pvxs::Kind::Compound)
                {
                    errorf(*logger_, "PV {} on reader {} returned non-compound payload; expected {}", pvName, name_, kPVXSDefaultMonitorRequest);
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.incrementReaderErrors(1.0, sourceTag);
                                });
                    break;
                }

                const pvxs::Value valueField = epics_value["value"];
                const pvxs::Value alarm = epics_value["alarm"];
                const pvxs::Value timestampField = epics_value["timeStamp"];
                if (!valueField.valid() || !alarm.valid() || !timestampField.valid())
                {
                    errorf(*logger_,
                           "PV {} on reader {} missing required fields for {}",
                           pvName,
                           name_,
                           kPVXSDefaultMonitorRequest);
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.incrementReaderErrors(1.0, sourceTag);
                                });
                    break;
                }

                IEventBusPush::EventBatch batch;
                batch.root_source = pvName;
                uint64_t epoch_seconds = 0;
                uint64_t nanoseconds = 0;
                if (const auto secondsField = timestampField["secondsPastEpoch"]; secondsField.valid())
                {
                    epoch_seconds = secondsField.as<uint64_t>();
                }
                else
                {
                    errorf(*logger_, "PV {} on reader {} missing required timeStamp.secondsPastEpoch", pvName, name_);
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.incrementReaderErrors(1.0, sourceTag);
                                });
                    break;
                }
                if (const auto nanosecondsField = timestampField["nanoseconds"]; nanosecondsField.valid())
                {
                    nanoseconds = nanosecondsField.as<uint64_t>();
                }
                else
                {
                    errorf(*logger_, "PV {} on reader {} missing required timeStamp.nanoseconds", pvName, name_);
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.incrementReaderErrors(1.0, sourceTag);
                                });
                    break;
                }

                auto event_value = IEventBusPush::MakeEventValue(epoch_seconds, nanoseconds);

                EpicsMLDPConversion::convertPVToProtoValue(valueField, &event_value->data_value);

                auto* valueStatus = event_value->data_value.mutable_valuestatus();

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
                    const int status = statusField.as<int>();
                    valueStatus->set_statuscode(status == 0 ? DataValue_ValueStatus_StatusCode_NO_STATUS
                                                            : DataValue_ValueStatus_StatusCode_RECORD_STATUS);
                }

                if (const auto messageField = alarm["message"]; messageField.valid())
                {
                    valueStatus->set_message(messageField.as<std::string>());
                }

                batch.tags.push_back(pvName);
                batch.values[pvName].emplace_back(std::move(event_value));
                emitted = 1;
                bus_->push(std::move(batch));
            }
            break;
        }

        const auto   processing_end = std::chrono::steady_clock::now();
        const double processing_ms = std::chrono::duration<double, std::milli>(processing_end - processing_start).count();
        metric_call(metrics_, [&](auto& m)
                    {
                        m.observeReaderProcessingTimeMs(processing_ms, sourceTag);
                    });

        if (emitted > 0)
        {
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
}
