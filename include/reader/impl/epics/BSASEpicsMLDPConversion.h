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
 * @file BSASEpicsMLDPConversion.h
 * @brief SLAC BSAS-specific NTTable conversion utilities for PVXS.
 *
 * This header provides the BSASEpicsMLDPConversion class for handling SLAC Beam
 * Synchronous Acquisition System (BSAS) NTTable structures. The class extends
 * EpicsMLDPConversion with specialized handling for row-timestamped NTTable data,
 * where each row carries its own timestamp extracted from designated columns.
 */

#pragma once

#include <BS_thread_pool.hpp>
#include <functional>
#include <pvxs/client.h>
#include <reader/impl/epics/EpicsMLDPConversion.h>
#include <string>
#include <util/bus/IDataBus.h>

#include <util/log/Logger.h>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Conversion class for SLAC BSAS NTTable structures using PVXS.
 *
 * This class extends EpicsMLDPConversion to provide specialized handling for
 * SLAC's Beam Synchronous Acquisition System (BSAS) data format. BSAS produces
 * NTTable structures where each row represents a time-synchronized sample across
 * multiple channels, with per-row timestamps stored in designated columns.
 *
 * The class supports three modes of operation:
 * - Direct batch output: Populates a single EventBatch with all columns
 * - Streaming column emission: Calls a callback for each column as it's processed
 * - Parallel processing: Uses a thread pool for concurrent column conversion
 *
 * @note This class is designed for PVXS (PV Access). For EPICS Base pvData
 *       NTTable handling, see EpicsPVDataConversion.
 *
 * @see EpicsMLDPConversion for base conversion functionality
 * @see EpicsPVDataConversion for EPICS Base equivalents
 */
class BSASEpicsMLDPConversion : public EpicsMLDPConversion
{
public:
    /**
     * @brief Callback function type for streaming column emission.
     *
     * This function is called once for each data column in the NTTable, allowing
     * the caller to process or batch columns incrementally rather than waiting
     * for the entire table to be converted.
     *
     * @param colName The name of the column being emitted.
     * @param events Vector of EventValue objects, one per row in the table,
     *               each carrying the column's value and the row's timestamp.
     */
    using ColumnEmitFn = std::function<void(std::string colName,
                                            std::vector<util::bus::IDataBus::EventValue> events)>;

    /**
     * @brief Convert an NTTable with row timestamps to an EventBatch.
     *
     * Processes a BSAS-style NTTable where each row has its own timestamp,
     * extracting the timestamp from designated columns and converting each
     * data column into a series of timestamped events.
     *
     * @param[in] log Logger for diagnostic output.
     * @param[in] tablePvName Name of the source PV (used for tagging/logging).
     * @param[in] epicsValue The PVXS Value containing the NTTable structure.
     * @param[in] tsSecondsField Name of the column containing epoch seconds.
     * @param[in] tsNanosField Name of the column containing nanoseconds.
     * @param[out] outBatch Pointer to the EventBatch to populate with converted data.
     * @param[out] outEmitted Number of events successfully emitted.
     *
     * @return true if conversion succeeded, false on error (check logs for details).
     *
     * @pre @p outBatch must point to a valid, initialized EventBatch.
     * @post On success, @p outBatch contains one entry per data column, each
     *       with one EventValue per table row.
     */
    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&                    log,
                                          const std::string&                                       tablePvName,
                                          const pvxs::Value&                                       epicsValue,
                                          const std::string&                                       tsSecondsField,
                                          const std::string&                                       tsNanosField,
                                          util::bus::IDataBus::EventBatch*                    outBatch,
                                          size_t&                                                  outEmitted);

    /**
     * @brief Convert an NTTable with row timestamps using streaming column emission.
     *
     * Similar to the batch variant, but instead of populating a single output
     * structure, this method calls the provided callback for each column. This
     * allows incremental processing and memory-efficient handling of large tables.
     *
     * @param[in] log Logger for diagnostic output.
     * @param[in] tablePvName Name of the source PV (used for tagging/logging).
     * @param[in] epicsValue The PVXS Value containing the NTTable structure.
     * @param[in] tsSecondsField Name of the column containing epoch seconds.
     * @param[in] tsNanosField Name of the column containing nanoseconds.
     * @param[in] emitColumn Callback invoked for each converted data column.
     * @param[out] outEmitted Number of events successfully emitted.
     *
     * @return true if conversion succeeded, false on error.
     */
    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                          const std::string&                    tablePvName,
                                          const pvxs::Value&                    epicsValue,
                                          const std::string&                    tsSecondsField,
                                          const std::string&                    tsNanosField,
                                          ColumnEmitFn                          emitColumn,
                                          size_t&                               outEmitted);

    /**
     * @brief Convert an NTTable with row timestamps using parallel processing.
     *
     * This variant uses a thread pool to process columns in parallel, which can
     * significantly improve throughput for tables with many columns or when
     * conversion involves expensive operations.
     *
     * @param[in] log Logger for diagnostic output.
     * @param[in] tablePvName Name of the source PV (used for tagging/logging).
     * @param[in] epicsValue The PVXS Value containing the NTTable structure.
     * @param[in] tsSecondsField Name of the column containing epoch seconds.
     * @param[in] tsNanosField Name of the column containing nanoseconds.
     * @param[in] emitColumn Callback invoked for each converted data column.
     *                       Must be thread-safe if @p pool is non-null.
     * @param[out] outEmitted Number of events successfully emitted.
     * @param[in] pool Optional thread pool for parallel column processing.
     *                 Pass nullptr for sequential processing.
     *
     * @return true if conversion succeeded, false on error.
     *
     * @note When @p pool is provided, the @p emitColumn callback may be invoked
     *       concurrently from multiple threads. Ensure thread safety in the callback.
     */
    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger& log,
                                          const std::string&                    tablePvName,
                                          const pvxs::Value&                    epicsValue,
                                          const std::string&                    tsSecondsField,
                                          const std::string&                    tsNanosField,
                                          ColumnEmitFn                          emitColumn,
                                          size_t&                               outEmitted,
                                          BS::light_thread_pool*                pool);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
