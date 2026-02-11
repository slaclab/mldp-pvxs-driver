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
#include <format>

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
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    for (const auto& pv : pvNames)
    {
        auto monitor = pva_context_.monitor(pv);
        auto pv_mon = monitor
                          .pvRequest(std::string(kPVXSDefaultMonitorRequest))
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
                                     }
                                 })
                          .exec();
        m_pva_subscriptions.push(pv_mon);
        infof(*logger_, "[{}/{}] Started monitoring", name_, pv);
    }
}

void EpicsPVXSReader::logAndRecordError(const std::string& message, const prometheus::Labels& tags)
{
    errorf(*logger_, "{}", message);
    metric_call(metrics_, [&](auto& m)
                {
                    m.incrementReaderErrors(1.0, tags);
                });
}

void EpicsPVXSReader::processDefaultMode(const std::string& pvName, const pvxs::Value& epicsValue, std::size_t& emitted)
{
    const prometheus::Labels sourceTag{{"source", pvName}};

    if (epicsValue.type().kind() != pvxs::Kind::Compound)
    {
        logAndRecordError(
            std::format("PV {} on reader {} returned non-compound payload; expected {}",
                        pvName, name_, kPVXSDefaultMonitorRequest),
            sourceTag);
        return;
    }

    const pvxs::Value valueField = epicsValue["value"];
    const pvxs::Value alarm = epicsValue["alarm"];
    const pvxs::Value timestampField = epicsValue["timeStamp"];

    if (!valueField.valid() || !alarm.valid() || !timestampField.valid())
    {
        logAndRecordError(
            std::format("PV {} on reader {} missing required fields for {}",
                        pvName, name_, kPVXSDefaultMonitorRequest),
            sourceTag);
        return;
    }

    const auto secondsField = timestampField["secondsPastEpoch"];
    if (!secondsField.valid())
    {
        logAndRecordError(
            std::format("PV {} on reader {} missing required timeStamp.secondsPastEpoch", pvName, name_),
            sourceTag);
        return;
    }
    const uint64_t epoch_seconds = secondsField.as<uint64_t>();

    const auto nanosecondsField = timestampField["nanoseconds"];
    if (!nanosecondsField.valid())
    {
        logAndRecordError(
            std::format("PV {} on reader {} missing required timeStamp.nanoseconds", pvName, name_),
            sourceTag);
        return;
    }
    const uint64_t nanoseconds = nanosecondsField.as<uint64_t>();

    auto  event_value = IEventBusPush::MakeEventValue(epoch_seconds, nanoseconds);
    auto* valueStatus = event_value->data_value.mutable_valuestatus();

    EpicsMLDPConversion::convertPVToProtoValue(valueField, &event_value->data_value);

    if (const auto severityField = alarm["severity"]; severityField.valid())
    {
        const int sev = severityField.as<int>();
        switch (sev)
        {
        case kAlarmSeverityNone:
            valueStatus->set_severity(DataValue_ValueStatus_Severity_NO_ALARM);
            break;
        case kAlarmSeverityMinor:
            valueStatus->set_severity(DataValue_ValueStatus_Severity_MINOR_ALARM);
            break;
        case kAlarmSeverityMajor:
            valueStatus->set_severity(DataValue_ValueStatus_Severity_MAJOR_ALARM);
            break;
        case kAlarmSeverityInvalid:
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

    IEventBusPush::EventBatch batch;
    batch.root_source = pvName;
    batch.tags.push_back(pvName);
    batch.values[pvName].emplace_back(std::move(event_value));
    emitted = 1;
    bus_->push(std::move(batch));
}

void EpicsPVXSReader::processSlacBsasTableMode(const std::string&     pvName,
                                               const pvxs::Value&     epicsValue,
                                               const PVRuntimeConfig* runtimeCfg,
                                               std::size_t&           emitted)
{
    const prometheus::Labels sourceTag{{"source", pvName}};
    const std::size_t        colBatchSize = config_.columnBatchSize();

    IEventBusPush::EventBatch tableBatch;
    tableBatch.root_source = pvName;
    tableBatch.tags.push_back(pvName);
    std::size_t colsInBatch = 0;

    auto resetBatch = [&tableBatch, &pvName, &colsInBatch]()
    {
        tableBatch = IEventBusPush::EventBatch{};
        tableBatch.root_source = pvName;
        tableBatch.tags.push_back(pvName);
        colsInBatch = 0;
    };

    if (!BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(
            *logger_, pvName, epicsValue,
            runtimeCfg ? runtimeCfg->tsSecondsField : "secondsPastEpoch",
            runtimeCfg ? runtimeCfg->tsNanosField : "nanoseconds",
            [&](std::string colName, std::vector<IEventBusPush::EventValue> events)
            {
                tableBatch.values[std::move(colName)] = std::move(events);
                ++colsInBatch;
                if (colBatchSize > 0 && colsInBatch >= colBatchSize)
                {
                    bus_->push(std::move(tableBatch));
                    resetBatch();
                }
            },
            emitted,
            reader_pool_->get_thread_count() > 1 ? reader_pool_.get() : nullptr))
    {
        logAndRecordError(
            std::format("Error converting PV {} to MLDP SLAC BSAS table batch on reader {}.", pvName, name_),
            sourceTag);
    }
    else if (!tableBatch.values.empty())
    {
        bus_->push(std::move(tableBatch));
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

        std::size_t emitted = 0;

        switch (mode)
        {
        case PVRuntimeConfig::Mode::SlacBsasTable:
            processSlacBsasTableMode(pvName, epics_value, runtimeCfg, emitted);
            break;

        case PVRuntimeConfig::Mode::Default:
        default:
            processDefaultMode(pvName, epics_value, emitted);
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
                            m.incrementReaderEvents(1.0, sourceTag);
                        });
            tracef(*logger_, "[{}/{}] event published", name_, pvName);
        }
    }
    catch (const pvxs::client::RemoteError& e)
    {
        logAndRecordError(
            std::format("Server error when reading PV {} on reader {}: {}", pvName, name_, e.what()),
            sourceTag);
    }
    catch (const std::exception& e)
    {
        logAndRecordError(
            std::format("Unexpected error processing PV {} on reader {}: {}", pvName, name_, e.what()),
            sourceTag);
    }
}
