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

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/impl/epics/base/EpicsPVDataConversion.h>
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
} // namespace

/// Construct the reader: build an EpicsReaderConfig from @p cfg, create the
/// named logger, then immediately begin monitoring all configured PV names.
EpicsBaseReader::EpicsBaseReader(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 const config::Config&                cfg)
    : EpicsReaderBase(
          std::move(bus),
          std::move(metrics),
          EpicsReaderConfig(cfg),
          makeLogger(cfg.get("name")))
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
        config_.monitorPollThreads(),
        config_.monitorPollIntervalMs(),
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
    batch.root_source = pvName;
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
        EpicsPVDataConversion::convertPVToDataBatch(*valueField, &batch_frame, pvName);
    }
    batch_frame.timestamps.push_back(TimestampEntry{epoch_seconds, nanoseconds});
    batch.tags.push_back(pvName);
    batch.frames.push_back(std::move(batch_frame));
    emitted = 1;
    batch.reader_name = name();
    bus_->push(std::move(batch));
}

/// Process a PV update in SlacBsasTable (NTTable row-timestamp) mode.
///
/// Delegates conversion to EpicsPVDataConversion::tryBuildNtTableRowTsBatch.
/// Columns are flushed to the bus in batches of at most
/// config_.columnBatchSize() entries to bound memory usage for wide tables.
/// @p emitted receives the total number of data rows published across all columns.
void EpicsBaseReader::processSlacBsasTableMode(const std::string&                     pvName,
                                               const ::epics::pvData::PVStructurePtr& epicsValue,
                                               const PVRuntimeConfig*                 runtimeCfg,
                                               std::size_t&                           emitted)
{
    const prometheus::Labels sourceTag{{"source", pvName}};
    const std::size_t        colBatchSize = config_.columnBatchSize();

    IDataBus::EventBatch tableBatch;
    tableBatch.root_source = pvName;
    tableBatch.tags.push_back(pvName);
    std::size_t colsInBatch = 0;

    auto resetBatch = [&tableBatch, &pvName, &colsInBatch]()
    {
        tableBatch = IDataBus::EventBatch{};
        tableBatch.root_source = pvName;
        tableBatch.tags.push_back(pvName);
        colsInBatch = 0;
    };

    if (!EpicsPVDataConversion::tryBuildNtTableRowTsBatch(
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
                    tableBatch.frames.push_back(std::move(frame));
                }
                ++colsInBatch;
                if (colBatchSize > 0 && colsInBatch >= colBatchSize)
                {
                    tableBatch.reader_name = name();
                    bus_->push(std::move(tableBatch));
                    resetBatch();
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
    else if (!tableBatch.frames.empty())
    {
        tableBatch.reader_name = name();
        bus_->push(std::move(tableBatch));
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
    catch (const std::exception& e)
    {
        errorf(*logger_, "Error when reading PV {} on reader {}: {}", pvName, name_, e.what());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementReaderErrors(1.0, sourceTag);
                    });
    }
}
