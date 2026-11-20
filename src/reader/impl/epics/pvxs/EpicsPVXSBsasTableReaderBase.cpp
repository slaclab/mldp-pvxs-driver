//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.h>

#include <metrics/Metrics.h>
#include <util/StringFormat.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <type_traits>
#include <variant>

using namespace mldp_pvxs_driver::reader::impl::epics;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::bus;

namespace {

/// Build a logger named "<readerType>[:<readerName>]" with '-' mapped to '_'.
std::shared_ptr<ILogger> makeTableLogger(std::string_view readerType, const std::string& readerName)
{
    std::string loggerName(readerType);
    std::replace(loggerName.begin(), loggerName.end(), '-', '_');
    if (!readerName.empty())
    {
        loggerName += ":";
        loggerName += readerName;
    }
    return newLogger(loggerName);
}

IDataBus::EventBatch makeTableEventBatch(const std::unordered_map<std::string, std::string>& metadata,
                                         const std::string&                                  readerName,
                                         const std::string&                                  pvName)
{
    IDataBus::EventBatch batch;
    batch.metadata = metadata;
    batch.metadata["source"] = pvName;
    batch.reader_name = readerName;
    batch.payload = TimeSeriesPayload{.root_source_name = pvName, .is_tabular = true};
    return batch;
}

/// Append rows [rowBegin, rowEnd) of every column of @p src to @p dst.
/// @p dst takes its timestamps from the first column appended; columns shorter
/// than @p rowBegin are skipped.
void appendRows(const DataBatch& src, std::size_t rowBegin, std::size_t rowEnd, DataBatch& dst)
{
    const std::size_t srcRows = src.timestamps.size();
    if (rowBegin >= srcRows)
    {
        return;
    }
    rowEnd = std::min(rowEnd, srcRows);
    if (dst.timestamps.empty())
    {
        dst.timestamps.assign(src.timestamps.begin() + static_cast<std::ptrdiff_t>(rowBegin),
                              src.timestamps.begin() + static_cast<std::ptrdiff_t>(rowEnd));
    }
    for (const auto& col : src.columns)
    {
        DataColumn out;
        out.name = col.name;
        out.metadata = col.metadata;
        std::visit(
            [&](const auto& values)
            {
                using V = std::decay_t<decltype(values)>;
                const std::size_t end = std::min(rowEnd, values.size());
                const std::size_t begin = std::min(rowBegin, end);
                out.values = V(values.begin() + static_cast<std::ptrdiff_t>(begin),
                               values.begin() + static_cast<std::ptrdiff_t>(end));
            },
            col.values);
        dst.columns.push_back(std::move(out));
        if (const auto it = src.array_dims.find(col.name); it != src.array_dims.end())
        {
            dst.array_dims[col.name] = it->second;
        }
    }
}

} // namespace

EpicsPVXSBsasTableReaderBase::EpicsPVXSBsasTableReaderBase(std::shared_ptr<util::bus::IDataBus>       bus,
                                                           std::shared_ptr<metrics::Metrics>          metrics,
                                                           const config::Config&                      cfg,
                                                           EpicsPVXSBsasTableReaderConfig::Generation generation,
                                                           std::string_view                           readerType,
                                                           const ColumnFilterFactory&                 filterFactory)
    : EpicsPVXSBsasTableReaderBase(std::move(bus), std::move(metrics),
                                   EpicsPVXSBsasTableReaderConfig(cfg, generation, readerType),
                                   cfg, readerType, filterFactory)
{
}

EpicsPVXSBsasTableReaderBase::EpicsPVXSBsasTableReaderBase(std::shared_ptr<util::bus::IDataBus> bus,
                                                           std::shared_ptr<metrics::Metrics>    metrics,
                                                           EpicsPVXSBsasTableReaderConfig       tableCfg,
                                                           const config::Config&                cfg,
                                                           std::string_view                     readerType,
                                                           const ColumnFilterFactory&           filterFactory)
    : EpicsPVXSReader(std::move(bus), std::move(metrics), tableCfg, cfg,
                      makeTableLogger(readerType, tableCfg.name()), readerType, kTableMonitorRequest)
    , table_config_(std::move(tableCfg))
{
    if (filterFactory)
    {
        for (const auto& pv : table_config_.pvNames())
        {
            filters_.emplace(pv, filterFactory(table_config_.optionsFor(pv)));
        }
    }
}

EpicsPVXSBsasTableReaderBase::~EpicsPVXSBsasTableReaderBase()
{
    // Queued events use table_config_ / filters_: drain them before members go.
    stopProcessing();
}

void EpicsPVXSBsasTableReaderBase::processEvent(std::string pvName, pvxs::Value epics_value)
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

        processTable(pvName, epics_value, sourceTag, emitted);

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

void EpicsPVXSBsasTableReaderBase::pushToBus(IDataBus::EventBatch      batch,
                                             const std::string&        pvName,
                                             const prometheus::Labels& sourceTag)
{
    if (!bus_->push(std::move(batch)))
    {
        logAndRecordError(
            util::format_string("Bus rejected BSAS table batch for PV {} on reader {}; batch dropped", pvName, name_),
            sourceTag);
    }
}

/// Convert @p epicsValue, then cut it into DataBatches of up to
/// rows-per-data-batch rows x pvs-per-data-batch PV columns (row chunks in
/// time order, columns in table order).  An EventBatch is pushed every
/// data-batches-per-push DataBatches, then the end_of_batch_group marker.
void EpicsPVXSBsasTableReaderBase::processTable(const std::string&        pvName,
                                                const pvxs::Value&        epicsValue,
                                                const prometheus::Labels& sourceTag,
                                                std::size_t&              emitted)
{
    const auto& opts = table_config_.optionsFor(pvName);
    const auto& metadata = mergedMetadataFor(pvName);

    const auto  filterIt = filters_.find(pvName);
    const auto& filter = filterIt != filters_.end() ? filterIt->second : ColumnFilter{};

    // 1. Convert: one DataBatch per kept column, holding all rows.
    std::vector<DataBatch> columns;
    const bool             converted = BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch(
        *logger_, pvName, epicsValue, opts.tsSecondsField, opts.tsNanosField,
        [&](std::string colName, std::vector<DataBatch> batches)
        {
            for (auto& b : batches)
            {
                if (b.timestamps.empty())
                {
                    logAndRecordError(
                        util::format_string("Dropping BSAS column {} without timestamps on reader {}", colName, name_),
                        sourceTag);
                    continue;
                }
                if (!hasValidData(b))
                {
                    tracef(*logger_, "[{}/{}] column {} all-NaN — discarding", name_, pvName, colName);
                    continue;
                }
                columns.push_back(std::move(b));
            }
        },
        emitted,
        reader_pool_->get_thread_count() > 1 ? reader_pool_.get() : nullptr,
        filter);

    if (!converted)
    {
        logAndRecordError(
            util::format_string("Error converting PV {} to SLAC BSAS table batch on reader {}.", pvName, name_),
            sourceTag);
    }

    // 2. Slice into DataBatches and push.
    IDataBus::EventBatch tableBatch = makeTableEventBatch(metadata, name_, pvName);
    std::size_t          inBatch = 0;
    auto                 flushBatch = [&]()
    {
        if (inBatch == 0)
        {
            return;
        }
        pushToBus(std::move(tableBatch), pvName, sourceTag);
        tableBatch = makeTableEventBatch(metadata, name_, pvName);
        inBatch = 0;
    };

    if (converted && !columns.empty())
    {
        const std::size_t rowCount = columns.front().timestamps.size();
        const std::size_t rowStep = opts.rowsPerDataBatch > 0 ? opts.rowsPerDataBatch : rowCount;
        const std::size_t pvStep = opts.pvsPerDataBatch > 0 ? opts.pvsPerDataBatch : columns.size();

        for (std::size_t rowBegin = 0; rowBegin < rowCount; rowBegin += rowStep)
        {
            const std::size_t rowEnd = std::min(rowBegin + rowStep, rowCount);
            for (std::size_t colBegin = 0; colBegin < columns.size(); colBegin += pvStep)
            {
                const std::size_t colEnd = std::min(colBegin + pvStep, columns.size());
                DataBatch         dataBatch;
                for (std::size_t c = colBegin; c < colEnd; ++c)
                {
                    appendRows(columns[c], rowBegin, rowEnd, dataBatch);
                }
                if (dataBatch.columns.empty())
                {
                    continue;
                }
                std::get<TimeSeriesPayload>(tableBatch.payload).frames.push_back(std::move(dataBatch));
                ++inBatch;
                if (opts.dataBatchesPerPush > 0 && inBatch >= opts.dataBatchesPerPush)
                {
                    flushBatch();
                }
            }
        }
    }
    flushBatch();

    IDataBus::EventBatch marker = makeTableEventBatch(metadata, name_, pvName);
    std::get<TimeSeriesPayload>(marker.payload).end_of_batch_group = true;
    pushToBus(std::move(marker), pvName, sourceTag);
}
