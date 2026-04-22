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

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <reader/impl/epics/pvxs/BSASEpicsMLDPConversion.h>
#include <reader/impl/epics/pvxs/EpicsMLDPConversion.h>
#include <util/StringFormat.h>
#include <util/log/Logger.h>

#include <chrono>

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

/// Return true when the first timestamp entry of a DataBatch is non-zero.
bool hasTimestamp(const DataBatch& batch)
{
    return !batch.timestamps.empty();
}
} // namespace

/// Construct the reader: initialise the PVA context from the process environment
/// (EPICS_PVA_* variables) and immediately begin monitoring all PV names declared
/// in @p cfg.
EpicsPVXSReader::EpicsPVXSReader(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 const config::Config&                cfg)
    : EpicsReaderBase(std::move(bus), std::move(metrics), EpicsReaderConfig(cfg), makeLogger(cfg.get("name")))
{
    pva_context_ = pvxs::client::Context::fromEnv();
    addPV(pvNames());
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
void EpicsPVXSReader::processDefaultMode(const std::string& pvName, const pvxs::Value& epicsValue, std::size_t& emitted)
{
    const prometheus::Labels sourceTag{{"source", pvName}};

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

    IDataBus::EventBatch eventBatch;
    eventBatch.root_source = pvName;
    eventBatch.tags.push_back(pvName);
    eventBatch.frames.push_back(std::move(batch));
    emitted = 1;
    eventBatch.reader_name = name();
    bus_->push(std::move(eventBatch));
}

/// Process a PV update in SlacBsasTable mode (NTTable with per-row timestamps).
///
/// Delegates conversion to BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch.
/// Columns are emitted in batches of at most config_.columnBatchSize() entries;
/// each full batch is pushed to the bus immediately, keeping memory bounded for
/// wide tables.  The thread pool is used for parallel column conversion when it
/// has more than one worker thread.  @p emitted receives the total number of
/// data rows published across all columns.
void EpicsPVXSReader::processSlacBsasTableMode(const std::string&     pvName,
                                               const pvxs::Value&     epicsValue,
                                               const PVRuntimeConfig* runtimeCfg,
                                               std::size_t&           emitted)
{
    const prometheus::Labels sourceTag{{"source", pvName}};
    const std::size_t        colBatchSize = config_.columnBatchSize();

    IDataBus::EventBatch tableBatch;
    tableBatch.root_source = pvName;
    tableBatch.tags.push_back(pvName);
    tableBatch.is_tabular = true;
    std::size_t colsInBatch = 0;

    auto resetBatch = [&tableBatch, &pvName, &colsInBatch]()
    {
        tableBatch = IDataBus::EventBatch{};
        tableBatch.root_source = pvName;
        tableBatch.tags.push_back(pvName);
        tableBatch.is_tabular = true;
        colsInBatch = 0;
    };

    if (!BSASEpicsMLDPConversion::tryBuildNtTableRowTsBatch(
            *logger_, pvName, epicsValue,
            runtimeCfg ? runtimeCfg->tsSecondsField : "secondsPastEpoch",
            runtimeCfg ? runtimeCfg->tsNanosField : "nanoseconds",
            [&](std::string colName, std::vector<DataBatch> batches)
            {
                for (auto& b : batches)
                {
                    if (!hasTimestamp(b))
                    {
                        logAndRecordError(
                            util::format_string("Dropping BSAS frame without timestamps for column {} on reader {}", colName, name_),
                            sourceTag);
                        continue;
                    }
                    tableBatch.frames.push_back(std::move(b));
                }
                ++colsInBatch;
                if (colBatchSize > 0 && colsInBatch >= colBatchSize)
                {
                    tableBatch.reader_name = name();
                    bus_->push(std::move(tableBatch));
                    resetBatch();
                }
            },
            emitted,
            reader_pool_->get_thread_count() > 1 ? reader_pool_.get() : nullptr))
    {
        logAndRecordError(
            util::format_string("Error converting PV {} to MLDP SLAC BSAS table batch on reader {}.", pvName, name_),
            sourceTag);
    }
    else if (!tableBatch.frames.empty())
    {
        tableBatch.reader_name = name();
        bus_->push(std::move(tableBatch));
    }

    // Signal end of this NTTable update round so downstream writers (e.g. HDF5)
    // know all column batches have been emitted and can flush accumulated state.
    IDataBus::EventBatch markerBatch;
    markerBatch.root_source = pvName;
    markerBatch.tags.push_back(pvName);
    markerBatch.is_tabular = true;
    markerBatch.end_of_batch_group = true;
    markerBatch.reader_name = name();
    bus_->push(std::move(markerBatch));
}

/// Entry point for every PVXS update, called from the reader thread pool.
///
/// Looks up the runtime configuration for @p pvName to determine the processing
/// mode, dispatches to processDefaultMode() or processSlacBsasTableMode(), and
/// records per-event processing-time and event-count metrics.  PVXS remote
/// errors and unexpected exceptions are caught, logged, and counted so that a
/// single bad update cannot disrupt the monitoring loop.
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
