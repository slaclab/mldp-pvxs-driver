//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsPVXSReader.h>
#include <reader/impl/epics/pvxs/EpicsPVXSReaderConfig.h>
#include <reader/impl/epics/shared/PvxsClientConfig.h>

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/impl/epics/pvxs/EpicsMLDPConversion.h>
#include <util/StringFormat.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::util::bus;

namespace {
/// Build a logger named "epics_pvxs_reader[:<readerName>]".
/// If @p readerName is empty the suffix is omitted.
std::shared_ptr<ILogger> makeLogger(const std::string& readerName)
{
    std::string loggerName = "epics_pvxs_reader";
    if (!readerName.empty())
    {
        loggerName += ":";
        loggerName += readerName;
    }
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}

} // namespace

/// Build the scalar-reader config view, rejecting BSAS table options that now
/// belong to the dedicated table readers.
EpicsPVXSReaderConfig EpicsPVXSReader::makeScalarConfig(const config::Config& cfg)
{
    EpicsPVXSReaderConfig readerCfg(cfg);
    for (const auto& pv : readerCfg.pvs())
    {
        if (pv.nttableRowTs.has_value())
        {
            throw EpicsReaderConfig::Error(util::format_string(
                "pvs[].option.type 'slac-bsas-table' (pv '{}') is not supported by reader type 'epics-pvxs'; "
                "use reader type 'epics-pvxs-bsas-gen1' (or 'epics-pvxs-bsas-gen2' for Gen2 tables)",
                pv.name));
        }
    }
    return readerCfg;
}

/// Construct the scalar reader: initialise the PVA context from the process
/// environment (EPICS_PVA_* variables) and immediately begin monitoring all PV
/// names declared in @p cfg.
EpicsPVXSReader::EpicsPVXSReader(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 const config::Config&                cfg)
    : EpicsPVXSReader(std::move(bus), std::move(metrics), makeScalarConfig(cfg), cfg,
                      makeLogger(cfg.get("name")), "epics-pvxs", kPVXSDefaultMonitorRequest)
{
    addPV(pvNames());
}

EpicsPVXSReader::EpicsPVXSReader(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 const EpicsReaderConfig&             readerCfg,
                                 const config::Config&                rawCfg,
                                 std::shared_ptr<util::log::ILogger>  logger,
                                 std::string_view                     clientTag,
                                 std::string_view                     monitorRequest)
    : EpicsReaderBase(std::move(bus), std::move(metrics), readerCfg, std::move(logger))
    , monitor_request_(monitorRequest)
{
    pva_context_ = PvxsClientConfig::buildContext(rawCfg, std::string(clientTag));
}

EpicsPVXSReader::~EpicsPVXSReader()
{
    stopProcessing();
}

void EpicsPVXSReader::stopProcessing()
{
    running_ = false;
    if (reader_pool_)
    {
        reader_pool_->wait();
    }
}

std::size_t EpicsPVXSReader::estimatePvxsValueBytes(const pvxs::Value& v)
{
    if (!v.valid()) return 0;
    switch (v.type().code)
    {
    case pvxs::TypeCode::Bool:    return 1;
    case pvxs::TypeCode::Int8:
    case pvxs::TypeCode::UInt8:   return 1;
    case pvxs::TypeCode::Int16:
    case pvxs::TypeCode::UInt16:  return 2;
    case pvxs::TypeCode::Int32:
    case pvxs::TypeCode::UInt32:
    case pvxs::TypeCode::Float32: return 4;
    case pvxs::TypeCode::Int64:
    case pvxs::TypeCode::UInt64:
    case pvxs::TypeCode::Float64: return 8;
    case pvxs::TypeCode::String:  return v.as<std::string>().size();
    case pvxs::TypeCode::BoolA:   return v.as<pvxs::shared_array<const bool>>().size();
    case pvxs::TypeCode::Int8A:
    case pvxs::TypeCode::UInt8A:  return v.as<pvxs::shared_array<const uint8_t>>().size();
    case pvxs::TypeCode::Int16A:
    case pvxs::TypeCode::UInt16A: return v.as<pvxs::shared_array<const uint16_t>>().size() * 2;
    case pvxs::TypeCode::Int32A:
    case pvxs::TypeCode::UInt32A:
    case pvxs::TypeCode::Float32A: return v.as<pvxs::shared_array<const int32_t>>().size() * 4;
    case pvxs::TypeCode::Int64A:
    case pvxs::TypeCode::UInt64A:
    case pvxs::TypeCode::Float64A: return v.as<pvxs::shared_array<const int64_t>>().size() * 8;
    case pvxs::TypeCode::StringA:
    {
        std::size_t total = 0;
        for (const auto& s : v.as<pvxs::shared_array<const std::string>>())
            total += s.size();
        return total;
    }
    case pvxs::TypeCode::Struct:
    case pvxs::TypeCode::Union:
    case pvxs::TypeCode::Any:
    {
        std::size_t total = 0;
        for (const auto& child : v.ichildren())
            total += estimatePvxsValueBytes(child);
        return total;
    }
    default: return 0;
    }
}

bool EpicsPVXSReader::hasValidData(const DataBatch& batch)
{
    if (batch.timestamps.empty())
        return false;

    bool hasNumeric = false;
    for (const auto& col : batch.columns)
    {
        bool valid = std::visit(
            [&hasNumeric](const auto& vals) -> bool
            {
                using T = std::decay_t<decltype(vals)>;
                if constexpr (std::is_same_v<T, std::vector<float>> ||
                              std::is_same_v<T, std::vector<double>>)
                {
                    hasNumeric = true;
                    return std::any_of(vals.begin(), vals.end(),
                                       [](auto v) { return !std::isnan(v); });
                }
                return false;
            },
            col.values);
        if (valid)
            return true;
    }
    return !hasNumeric;
}

/// Subscribe to PVXS channel-access monitors for each PV in @p pvNames.
/// Each incoming value is drained from the subscription queue in the PVXS
/// network thread, a receive-event metric is recorded, and the value is then
/// offloaded to the reader thread pool for actual conversion and bus delivery.
void EpicsPVXSReader::addPV(const PVSet& pvNames)
{
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    for (const auto& pv : pvNames)
    {
        auto monitor = pva_context_.monitor(pv);
        auto pv_mon = monitor
                          .pvRequest(monitor_request_)
                          .event([this](pvxs::client::Subscription& s)
                                 {
                                     for (pvxs::Value value = s.pop(); value; value = s.pop())
                                     {
                                         metric_call(metrics_, [&](auto& m)
                                                     {
                                                         m.incrementReaderEventsReceived(1.0, {{"source", s.name()}});
                                                     });
                                         std::string pvName = s.name();

                                         // pv event is processed into another thread to not block the PVXS network thread,
                                         // which could cause monitor disconnects if processing takes too long
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

/// Log @p message at error level and increment the reader-error metric counter
/// using @p tags as the label set.
void EpicsPVXSReader::logAndRecordError(const std::string& message, const prometheus::Labels& tags)
{
    errorf(*logger_, "{}", message);
    metric_call(metrics_, [&](auto& m)
                {
                    m.incrementReaderErrors(1.0, tags);
                });
}

/// Process a single PV update in Default mode (non-BSAS, non-table).
///
/// Expects a compound PVXS value containing "value", "alarm", and "timeStamp"
/// sub-fields.  "timeStamp" must carry "secondsPastEpoch" and "nanoseconds".
/// Compound (nested) value fields are rejected with a warning because Default
/// mode is designed for scalar / scalar-array payloads only.
/// On success, one EventBatch with a single DataBatch is pushed to the bus
/// and @p emitted is set to 1.
void EpicsPVXSReader::processDefaultMode(const std::string& pvName, const pvxs::Value& epicsValue,
                                         const prometheus::Labels& sourceTag, std::size_t& emitted)
{
    if (epicsValue.type().kind() != pvxs::Kind::Compound)
    {
        logAndRecordError(
            util::format_string("PV {} on reader {} returned non-compound payload; expected {}", pvName, name_, kPVXSDefaultMonitorRequest),
            sourceTag);
        return;
    }

    const pvxs::Value valueField = epicsValue["value"];
    const pvxs::Value alarm = epicsValue["alarm"];
    const pvxs::Value timestampField = epicsValue["timeStamp"];

    if (!valueField.valid() || !alarm.valid() || !timestampField.valid())
    {
        logAndRecordError(
            util::format_string("PV {} on reader {} missing required fields for {}", pvName, name_, kPVXSDefaultMonitorRequest),
            sourceTag);
        return;
    }

    const auto secondsField = timestampField["secondsPastEpoch"];
    if (!secondsField.valid())
    {
        logAndRecordError(
            util::format_string("PV {} on reader {} missing required timeStamp.secondsPastEpoch", pvName, name_),
            sourceTag);
        return;
    }
    const uint64_t epoch_seconds = secondsField.as<uint64_t>();

    const auto nanosecondsField = timestampField["nanoseconds"];
    if (!nanosecondsField.valid())
    {
        logAndRecordError(
            util::format_string("PV {} on reader {} missing required timeStamp.nanoseconds", pvName, name_),
            sourceTag);
        return;
    }
    const uint64_t nanoseconds = nanosecondsField.as<uint64_t>();

    if (valueField.type().kind() == pvxs::Kind::Compound)
    {
        warnf(*logger_,
              "[{}/{}] PV has compound (non-scalar) value field in default mode — skipping",
              name_, pvName);
        return; // Do not push event to bus
    }

    DataBatch batch;
    batch.timestamps.push_back(TimestampEntry{epoch_seconds, nanoseconds});
    EpicsMLDPConversion::convertPVToDataBatch(valueField, &batch, pvName);

    if (!hasValidData(batch))
    {
        tracef(*logger_, "[{}/{}] discarding event: value is NaN", name_, pvName);
        return;
    }

    IDataBus::EventBatch eventBatch;
    eventBatch.metadata = mergedMetadataFor(pvName);
    eventBatch.reader_name = name();
    eventBatch.payload = TimeSeriesPayload{
        .root_source_name = pvName,
        .frames           = {std::move(batch)},
    };
    emitted = 1;
    bus_->push(std::move(eventBatch));
}

/// Record the reader pool queue depth gauge.
void EpicsPVXSReader::recordPoolDepth()
{
    metric_call(metrics_, [&](auto& m)
                {
                    m.setReaderPoolQueueDepth(
                        static_cast<double>(reader_pool_->get_tasks_queued()),
                        {{"reader", name_}});
                });
}

/// Record processing time and, for updates that emitted data, the event count,
/// total bytes and bytes/s for @p pvName.
void EpicsPVXSReader::recordEventMetrics(const std::string&                    pvName,
                                         const prometheus::Labels&             sourceTag,
                                         std::size_t                           dataBytes,
                                         std::size_t                           emitted,
                                         std::chrono::steady_clock::time_point processingStart)
{
    const auto   processing_end = std::chrono::steady_clock::now();
    const double processing_ms = std::chrono::duration<double, std::milli>(processing_end - processingStart).count();
    metric_call(metrics_, [&](auto& m)
                {
                    m.observeReaderProcessingTimeMs(processing_ms, sourceTag);
                });

    if (emitted == 0)
    {
        return;
    }

    metric_call(metrics_, [&](auto& m)
                {
                    m.incrementReaderEvents(1.0, sourceTag);
                });
    if (dataBytes > 0)
    {
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderDataBytesTotal(static_cast<double>(dataBytes), sourceTag);
                    });
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(last_event_time_mutex_);
            auto&                       last_time = last_event_time_[pvName];
            if (last_time != std::chrono::steady_clock::time_point{})
            {
                const double interval_ms = std::chrono::duration<double, std::milli>(now - last_time).count();
                if (interval_ms > 0.0)
                {
                    const double bps = (static_cast<double>(dataBytes) * 1000.0) / interval_ms;
                    metric_call(metrics_, [&](auto& m)
                                {
                                    m.setReaderDataBytesPerSecond(bps, sourceTag);
                                });
                }
            }
            last_time = now;
        }
    }
    tracef(*logger_, "[{}/{}] event published", name_, pvName);
}

/// Entry point for every PVXS update, called from the reader thread pool.
///
/// Converts the scalar / scalar-array update (Default mode) and records
/// per-event metrics.  PVXS remote errors and unexpected exceptions are
/// caught, logged, and counted so that a single bad update cannot disrupt the
/// monitoring loop.
void EpicsPVXSReader::processEvent(std::string pvName, pvxs::Value epics_value)
{
    if (!running_.load())
    {
        return;
    }

    recordPoolDepth();

    const prometheus::Labels sourceTag{{"source", pvName}};

    try
    {
        const auto        processing_start = std::chrono::steady_clock::now();
        const std::size_t dataBytes = estimatePvxsValueBytes(epics_value);
        std::size_t       emitted = 0;

        processDefaultMode(pvName, epics_value, sourceTag, emitted);

        recordEventMetrics(pvName, sourceTag, dataBytes, emitted, processing_start);
    }
    catch (const pvxs::client::RemoteError& e)
    {
        logAndRecordError(
            util::format_string("Server error when reading PV {} on reader {}: {}", pvName, name_, e.what()),
            sourceTag);
    }
    catch (const std::exception& e)
    {
        logAndRecordError(
            util::format_string("Unexpected error processing PV {} on reader {}: {}", pvName, name_, e.what()),
            sourceTag);
    }
}

