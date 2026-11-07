//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file EpicsPVXSBsasTableReaderBase.h
 * @brief Shared implementation of the PVXS SLAC BSAS NTTable readers.
 */

#pragma once

#include <reader/impl/epics/pvxs/BSASEpicsDataBatchConversion.h>
#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderConfig.h>
#include <reader/impl/epics/pvxs/EpicsPVXSReader.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Base class for readers that subscribe to SLAC BSAS NTTable PVs.
 *
 * Every NTTable update is converted into the same bus shape that
 * HDF5BsasGen1Reader produces for HDF5 files:
 * - one or more tabular EventBatch messages
 *   (`TimeSeriesPayload{.root_source_name = <table PV>, .is_tabular = true}`),
 *   each holding up to `data-batches-per-push` DataBatches;
 * - each DataBatch holds up to `rows-per-data-batch` rows of up to
 *   `pvs-per-data-batch` PV columns, sharing the row timestamps;
 * - then one `end_of_batch_group` marker.
 *
 * Columns without timestamps are dropped (counted as a reader error) and
 * all-NaN columns are discarded.  A PVXS stream cannot be paused, so a
 * rejected bus push is logged, counted as a reader error and dropped.
 *
 * Derived readers choose the table generation and may supply a column filter
 * factory; they must call addPV(pvNames()) at the end of their constructor.
 */
class EpicsPVXSBsasTableReaderBase : public EpicsPVXSReader
{
public:
    ~EpicsPVXSBsasTableReaderBase() override;

    /** @return Parsed table configuration. */
    const EpicsPVXSBsasTableReaderConfig& tableConfig() const
    {
        return table_config_;
    }

protected:
    using TableOptions = EpicsPVXSBsasTableReaderConfig::TableOptions;
    using ColumnFilter = BSASEpicsDataBatchConversion::ColumnFilter;
    /** @brief Builds the column filter for one PV from its table settings. */
    using ColumnFilterFactory = std::function<ColumnFilter(const TableOptions&)>;

    /** @brief pvRequest used by table readers: the whole NTTable structure. */
    static constexpr std::string_view kTableMonitorRequest = "field()";

    /**
     * @param bus Event bus.
     * @param metrics Metrics collector (may be null).
     * @param cfg Raw reader configuration.
     * @param generation Table generation (selects the accepted config keys).
     * @param readerType Reader type name (logs, config errors).
     * @param filterFactory Optional per-PV column filter factory; empty keeps
     *        every non-timestamp column.
     */
    EpicsPVXSBsasTableReaderBase(std::shared_ptr<util::bus::IDataBus>         bus,
                                 std::shared_ptr<metrics::Metrics>            metrics,
                                 const config::Config&                        cfg,
                                 EpicsPVXSBsasTableReaderConfig::Generation   generation,
                                 std::string_view                             readerType,
                                 const ColumnFilterFactory&                   filterFactory = {});

    /** @brief Convert one NTTable update and push it to the bus. */
    void processEvent(std::string pvName, pvxs::Value epics_value) override;

private:
    EpicsPVXSBsasTableReaderBase(std::shared_ptr<util::bus::IDataBus> bus,
                                 std::shared_ptr<metrics::Metrics>    metrics,
                                 EpicsPVXSBsasTableReaderConfig       tableCfg,
                                 const config::Config&                cfg,
                                 std::string_view                     readerType,
                                 const ColumnFilterFactory&           filterFactory);

    /**
     * @brief Convert @p epicsValue into tabular batches plus the group marker.
     * @param[out] emitted Number of rows converted across all columns.
     */
    void processTable(const std::string& pvName, const pvxs::Value& epicsValue,
                      const prometheus::Labels& sourceTag, std::size_t& emitted);

    /** @brief Push @p batch; a rejected push is logged and counted, not retried. */
    void pushToBus(util::bus::IDataBus::EventBatch batch, const std::string& pvName,
                   const prometheus::Labels& sourceTag);

    EpicsPVXSBsasTableReaderConfig                table_config_;
    std::unordered_map<std::string, ColumnFilter> filters_; ///< Per-PV column filter (absent = keep all).
};

} // namespace mldp_pvxs_driver::reader::impl::epics
