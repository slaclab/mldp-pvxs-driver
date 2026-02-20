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
 * @file EpicsPVDataConversion.h
 * @brief Conversion utilities for EPICS Base pvData to MLDP protobuf format.
 *
 * This header provides the EpicsPVDataConversion class for converting EPICS Base
 * pvData structures to the MLDP ingestion protobuf format. It includes support
 * for standard scalar/array PVs as well as SLAC BSAS NTTable structures.
 */

#pragma once

#include <ingestion.grpc.pb.h>
#include <pv/pvData.h>
#include <util/bus/IEventBusPush.h>
#include <util/log/Logger.h>

#include <functional>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Conversion utilities for EPICS Base pvData to MLDP protobuf format.
 *
 * This class provides static methods for converting EPICS Base pvData structures
 * (PVField, PVStructure) to the protobuf DataValue format used by the MLDP
 * ingestion service. It handles various EPICS data types and provides specialized
 * support for SLAC BSAS NTTable structures with per-row timestamps.
 *
 * @note This class is designed for EPICS Base (pvData). For PVXS conversions,
 *       see EpicsMLDPConversion and BSASEpicsMLDPConversion.
 *
 * @see EpicsMLDPConversion for PVXS conversions
 * @see EpicsBaseReader for the reader that uses these conversions
 */
class EpicsPVDataConversion
{
public:
    /**
     * @brief Callback function type for streaming column emission.
     *
     * This function is called once for each data column in an NTTable, allowing
     * incremental processing of large tables.
     *
     * @param colName The name of the column being emitted.
     * @param events Vector of EventValue objects, one per row in the table.
     */
    using ColumnEmitFn = std::function<void(std::string colName,
                                            std::vector<mldp_pvxs_driver::util::bus::IEventBusPush::EventValue> events)>;

    /**
     * @brief Convert an EPICS pvData field to an MLDP protobuf DataValue.
     *
     * Transforms the given pvData PVField into the corresponding protobuf
     * DataValue representation. Handles scalar types, array types, and
     * enumerated types.
     *
     * @param[in] pvField The pvData field to convert.
     * @param[out] protoValue Pointer to the DataValue to populate. Must not be null.
     *
     * @pre @p protoValue must point to a valid DataValue object.
     * @post @p protoValue contains the converted representation.
     */
    static void convertPVToProtoValue(const ::epics::pvData::PVField& pvField, DataValue* protoValue);

    /**
     * @brief Convert an NTTable with row timestamps to an EventBatch.
     *
     * Processes a BSAS-style NTTable where each row has its own timestamp,
     * extracting timestamps from designated columns and converting each
     * data column into a series of timestamped events.
     *
     * @param[in] log Logger for diagnostic output.
     * @param[in] tablePvName Name of the source PV (used for tagging/logging).
     * @param[in] epicsValue The pvData PVStructure containing the NTTable.
     * @param[in] tsSecondsField Name of the column containing epoch seconds.
     * @param[in] tsNanosField Name of the column containing nanoseconds.
     * @param[out] outBatch Pointer to the EventBatch to populate.
     * @param[out] outEmitted Number of events successfully emitted.
     *
     * @return true if conversion succeeded, false on error.
     *
     * @pre @p outBatch must point to a valid, initialized EventBatch.
     */
    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&                    log,
                                          const std::string&                                       tablePvName,
                                          const ::epics::pvData::PVStructurePtr&                   epicsValue,
                                          const std::string&                                       tsSecondsField,
                                          const std::string&                                       tsNanosField,
                                          mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch*  outBatch,
                                          size_t&                                                  outEmitted);

    /**
     * @brief Convert an NTTable with row timestamps using streaming column emission.
     *
     * Similar to the batch variant, but invokes a callback for each column
     * instead of populating a single output structure.
     *
     * @param[in] log Logger for diagnostic output.
     * @param[in] tablePvName Name of the source PV (used for tagging/logging).
     * @param[in] epicsValue The pvData PVStructure containing the NTTable.
     * @param[in] tsSecondsField Name of the column containing epoch seconds.
     * @param[in] tsNanosField Name of the column containing nanoseconds.
     * @param[in] emitColumn Callback invoked for each converted data column.
     * @param[out] outEmitted Number of events successfully emitted.
     *
     * @return true if conversion succeeded, false on error.
     */
    static bool tryBuildNtTableRowTsBatch(mldp_pvxs_driver::util::log::ILogger&  log,
                                          const std::string&                     tablePvName,
                                          const ::epics::pvData::PVStructurePtr& epicsValue,
                                          const std::string&                     tsSecondsField,
                                          const std::string&                     tsNanosField,
                                          ColumnEmitFn                           emitColumn,
                                          size_t&                                outEmitted);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
