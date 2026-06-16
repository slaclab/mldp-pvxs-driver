//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/base/EpicsBaseReader.h>
#include <reader/impl/epics/base/EpicsBaseReaderConfig.h>

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/impl/epics/base/EpicsPVDataBatchConversion.h>
#include <util/log/Logger.h>

#include <chrono>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::util::bus;

namespace {
/// Build a logger named "epics_base_reader[:<readerName>]".
/// If @p readerName is empty the suffix is omitted.
std::shared_ptr<ILogger> makeLogger(const std::string& readerName)
{
    std::string loggerName = "epics_base_reader";
    if (!readerName.empty())
    {
        loggerName += ":";
        loggerName += readerName;
    }
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}

bool hasTimestamps(const DataBatch& batch)
{
    return !batch.timestamps.empty();
}

std::size_t estimateEpicsBaseValueBytes(const ::epics::pvData::PVStructurePtr& v)
{
    if (!v) return 0;
    std::size_t total = 0;
    for (const auto& field : v->getPVFields())
    {
        if (!field) continue;
        switch (field->getField()->getType())
        {
        case ::epics::pvData::scalar:
        {
            auto sc = std::dynamic_pointer_cast<::epics::pvData::PVScalar>(field);
            if (!sc) break;
            switch (sc->getScalar()->getScalarType())
            {
            case ::epics::pvData::pvBoolean:
            case ::epics::pvData::pvByte:
            case ::epics::pvData::pvUByte:   total += 1; break;
            case ::epics::pvData::pvShort:
            case ::epics::pvData::pvUShort:  total += 2; break;
            case ::epics::pvData::pvInt:
            case ::epics::pvData::pvUInt:
            case ::epics::pvData::pvFloat:   total += 4; break;
            case ::epics::pvData::pvLong:
            case ::epics::pvData::pvULong:
            case ::epics::pvData::pvDouble:  total += 8; break;
            case ::epics::pvData::pvString:
            {
                auto s = std::dynamic_pointer_cast<::epics::pvData::PVString>(field);
                if (s) total += s->get().size();
                break;
            }
            default: break;
            }
            break;
        }
        case ::epics::pvData::scalarArray:
        {
            auto arr = std::dynamic_pointer_cast<::epics::pvData::PVScalarArray>(field);
            if (!arr) break;
            std::size_t n = arr->getLength();
            switch (arr->getScalarArray()->getElementType())
            {
            case ::epics::pvData::pvBoolean:
            case ::epics::pvData::pvByte:
            case ::epics::pvData::pvUByte:   total += n; break;
            case ::epics::pvData::pvShort:
            case ::epics::pvData::pvUShort:  total += n * 2; break;
            case ::epics::pvData::pvInt:
            case ::epics::pvData::pvUInt:
            case ::epics::pvData::pvFloat:   total += n * 4; break;
            case ::epics::pvData::pvLong:
            case ::epics::pvData::pvULong:
            case ::epics::pvData::pvDouble:  total += n * 8; break;
            default: break;
            }
            break;
        }
        case ::epics::pvData::structure:
        {
            auto sub = std::dynamic_pointer_cast<::epics::pvData::PVStructure>(field);
            if (sub) total += estimateEpicsBaseValueBytes(sub);
            break;
        }
        default: break;
        }
    }
    return total;
}
} // namespace

/// Construct the reader: build an EpicsReaderConfig from @p cfg, create the
/// named logger, then immediately begin monitoring all configured PV names.
EpicsBaseReader::EpicsBaseReader(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 const config::Config&                cfg)
    : EpicsReaderBase(
          std::move(bus),
          std::move(metrics),
          EpicsBaseReaderConfig(cfg),
          makeLogger(cfg.get("name")))
    , base_config_(cfg)
{
    addPV(pvNames());
}

/// Stop the reader: signal shutdown, then destroy the poller (joins its threads).
EpicsBaseReader::~EpicsBaseReader()
{
    running_ = false;
    epics_base_poller_.reset();
}

/// Create an EpicsBaseMonitorPoller for @p pvNames, configured with the poll
/// thread count and interval from the reader config.  The poller calls
/// drainEpicsBaseQueue() each time new data is available.
void EpicsBaseReader::addPV(const PVSet& pvNames)
{
    const std::vector<std::string> pv_list(pvNames.begin(), pvNames.end());
    epics_base_poller_ = std::make_unique<EpicsBaseMonitorPoller>(
        pv_list,
        base_config_.monitorPollThreads(),
        base_config_.monitorPollIntervalMs(),
        [this]()
        {
            drainEpicsBaseQueue();
        },
        logger_);
}

/// Drain all pending updates from the poller under the drain mutex.
/// For each update, increments the receive-event metric, then offloads
/// conversion and bus delivery to the reader thread pool so the poller
/// callback is never blocked by slow downstream processing.
void EpicsBaseReader::drainEpicsBaseQueue()
{
    if (!epics_base_poller_)
    {
        return;
    }

    std::lock_guard<std::mutex> lk(epics_base_drain_mutex_);
    epics_base_poller_->drain([this](const std::string& pvName, ::epics::pvData::PVStructurePtr value)
                              {
                                  metric_call(metrics_, [&](auto& m)
                                              {
                                                  m.incrementReaderEventsReceived(1.0, {{"source", pvName}});
                                              });
                                  std::string pv = pvName;
                                  reader_pool_->detach_task(
                                      [this, n = std::move(pv), v = std::move(value)]() mutable
                                      {
                                          processEvent(std::move(n), std::move(v));
                                      });
                                  metric_call(metrics_, [&](auto& m)
                                              {
                                                  m.setReaderPoolQueueDepth(
                                                      static_cast<double>(reader_pool_->get_tasks_queued()),
                                                      {{"reader", name_}});
                                              });
                              });
}

/// Process a single PV update in Default (scalar/array) mode.
///
/// Extracts "timeStamp.secondsPastEpoch" and "timeStamp.nanoseconds" from
/// @p epicsValue; falls back to wall-clock seconds when the timestamp is
/// absent.  Missing or compound "value" fields are rejected with a warning.
/// On success one EventBatch is pushed to the bus and @p emitted is set to 1.
void EpicsBaseReader::processDefaultMode(const std::string&                     pvName,
                                         const ::epics::pvData::PVStructurePtr& epicsValue,
                                         std::size_t&                           emitted)
{
    IDataBus::EventBatch batch;
    uint64_t epoch_seconds = 0;
    uint64_t nanoseconds = 0;
    bool     setEpoch = false;

    if (epicsValue)
    {
        if (auto timeStamp = epicsValue->getSubField<::epics::pvData::PVStructure>("timeStamp"))
        {
            if (auto secondsField = timeStamp->getSubField<::epics::pvData::PVScalar>("secondsPastEpoch"))
            {
                epoch_seconds = secondsField->getAs<uint64_t>();
                setEpoch = true;
            }
            if (auto nanosField = timeStamp->getSubField<::epics::pvData::PVScalar>("nanoseconds"))
            {
                nanoseconds = nanosField->getAs<uint64_t>();
            }
        }
    }
    if (!setEpoch)
    {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    }

    DataBatch batch_frame;

    if (epicsValue)
    {
        auto valueField = epicsValue->getSubField("value");
        if (!valueField)
        {
            warnf(*logger_,
                  "[{}/{}] PV has no 'value' field in default mode — skipping",
                  name_, pvName);
            return;
        }
        const bool isStructPayload =
            (std::dynamic_pointer_cast<::epics::pvData::PVStructure>(valueField) != nullptr);
        if (isStructPayload)
        {
            warnf(*logger_,
                  "[{}/{}] PV has compound (non-scalar) value field in default mode — skipping",
                  name_, pvName);
            return;
        }
        EpicsPVDataBatchConversion::convertPVToDataBatch(*valueField, &batch_frame, pvName);
    }
    batch_frame.timestamps.push_back(TimestampEntry{epoch_seconds, nanoseconds});
    // Build merged metadata: reader-level base, PV-level overrides
    auto merged = config_.staticMetadata();
    for (const auto& pv_cfg : config_.pvs())
    {
        if (pv_cfg.name == pvName)
        {
            for (auto& [k, v] : pv_cfg.metadata)
                merged[k] = v;
            break;
        }
    }
    batch.metadata = std::move(merged);
    batch.reader_name = name();
    batch.payload = TimeSeriesPayload{
        .root_source_name = pvName,
        .frames           = {std::move(batch_frame)},
    };
    emitted = 1;
    bus_->push(std::move(batch));
}

/// Process a PV update in SlacBsasTable (NTTable row-timestamp) mode.
///
/// Delegates conversion to EpicsPVDataBatchConversion::tryBuildNtTableRowTsBatch.
/// Columns are grouped into shared DataBatch frames according to the per-PV
/// columnBatchSize setting (default 1 = each column gets its own frame).
/// Full EventBatch messages are pushed to the bus when global columnBatchSize
/// columns have been accumulated.
void EpicsBaseReader::processSlacBsasTableMode(const std::string&                     pvName,
                                               const ::epics::pvData::PVStructurePtr& epicsValue,
                                               const PVRuntimeConfig*                 runtimeCfg,
                                               std::size_t&                           emitted)
{
    const prometheus::Labels sourceTag{{"source", pvName}};
    const std::size_t        busBatchSize = config_.columnBatchSize();
    const std::size_t        perPvBatchSize = runtimeCfg ? runtimeCfg->columnBatchSize : 1;

    // Build merged metadata: reader-level base, PV-level overrides
    auto merged_meta = config_.staticMetadata();
    for (const auto& pv_cfg : config_.pvs())
    {
        if (pv_cfg.name == pvName)
        {
            for (auto& [k, v] : pv_cfg.metadata)
                merged_meta[k] = v;
            break;
        }
    }

    IDataBus::EventBatch tableBatch;
    tableBatch.metadata = merged_meta;
    tableBatch.payload  = TimeSeriesPayload{.root_source_name = pvName, .is_tabular = true};
    std::size_t colsInBatch = 0;

    // Accumulator for grouping multiple columns into one shared-timestamp DataBatch frame.
    DataBatch   currentFrame;
    std::size_t colsInCurrentFrame = 0;
    bool        frameTimestampsSet = false;

    auto resetBatch = [&tableBatch, &pvName, &colsInBatch, merged_meta]()
    {
        tableBatch = IDataBus::EventBatch{};
        tableBatch.metadata = merged_meta;
        tableBatch.payload  = TimeSeriesPayload{.root_source_name = pvName, .is_tabular = true};
        colsInBatch = 0;
    };

    auto flushCurrentFrame = [&]()
    {
        if (colsInCurrentFrame == 0)
            return;
        std::get<TimeSeriesPayload>(tableBatch.payload).frames.push_back(std::move(currentFrame));
        currentFrame = DataBatch{};
        colsInCurrentFrame = 0;
        frameTimestampsSet = false;
        ++colsInBatch;
        if (busBatchSize > 0 && colsInBatch >= busBatchSize)
        {
            tableBatch.reader_name = name();
            bus_->push(std::move(tableBatch));
            resetBatch();
        }
    };

    if (!EpicsPVDataBatchConversion::tryBuildNtTableRowTsBatch(
            *logger_, pvName, epicsValue,
            runtimeCfg ? runtimeCfg->tsSecondsField : "secondsPastEpoch",
            runtimeCfg ? runtimeCfg->tsNanosField : "nanoseconds",
            [&](std::string colName, std::vector<DataBatch> frames)
            {
                for (auto& frame : frames)
                {
                    if (!hasTimestamps(frame))
                    {
                        errorf(*logger_, "Dropping BSAS frame without timestamps for column {} on reader {}", colName, name_);
                        metric_call(metrics_, [&](auto& m)
                                    {
                                        m.incrementReaderErrors(1.0, sourceTag);
                                    });
                        continue;
                    }

                    if (!frameTimestampsSet)
                    {
                        currentFrame.timestamps = std::move(frame.timestamps);
                        frameTimestampsSet = true;
                    }
                    for (auto& col : frame.columns)
                        currentFrame.columns.push_back(std::move(col));
                    for (auto& [k, v] : frame.array_dims)
                        currentFrame.array_dims[k] = v;
                }

                ++colsInCurrentFrame;
                if (perPvBatchSize > 0 && colsInCurrentFrame >= perPvBatchSize)
                {
                    flushCurrentFrame();
                }
            },
            emitted))
    {
        errorf(*logger_, "Error converting PV {} to MLDP SLAC BSAS table batch on reader {}.", pvName, name_);
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0, sourceTag);
                    });
    }
    else
    {
        // Flush any remaining accumulated columns in current frame.
        flushCurrentFrame();
        if (!std::get<TimeSeriesPayload>(tableBatch.payload).frames.empty())
        {
            tableBatch.reader_name = name();
            bus_->push(std::move(tableBatch));
        }
    }
}

/// Entry point for every EPICS Base update, called from the reader thread pool.
///
/// Looks up the runtime mode for @p pvName and dispatches to
/// processDefaultMode() or processSlacBsasTableMode(). Records per-event
/// processing-time and event-count metrics. Exceptions are caught, logged,
/// and counted so that a single bad update cannot disrupt the monitoring loop.
void EpicsBaseReader::processEvent(std::string pvName, ::epics::pvData::PVStructurePtr epics_value)
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

        const std::size_t dataBytes = estimateEpicsBaseValueBytes(epics_value);
        std::size_t       emitted   = 0;

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
            if (dataBytes > 0)
            {
                metric_call(metrics_, [&](auto& m) {
                    m.incrementReaderDataBytesTotal(static_cast<double>(dataBytes), sourceTag);
                });
                const auto now = std::chrono::steady_clock::now();
                auto& last_time = last_event_time_[pvName];
                if (last_time != std::chrono::steady_clock::time_point{})
                {
                    const double interval_ms = std::chrono::duration<double, std::milli>(
                        now - last_time).count();
                    if (interval_ms > 0.0)
                    {
                        const double bps = (static_cast<double>(dataBytes) * 1000.0) / interval_ms;
                        metric_call(metrics_, [&](auto& m) {
                            m.setReaderDataBytesPerSecond(bps, sourceTag);
                        });
                    }
                }
                last_time = now;
            }
            tracef(*logger_, "[{}/{}] event published", name_, pvName);
        }
    }
    catch (const std::exception& e)
    {
        errorf(*logger_, "Error when reading PV {} on reader {}: {}", pvName, name_, e.what());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0, sourceTag);
                    });
    }
}
