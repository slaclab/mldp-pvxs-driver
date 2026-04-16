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
 * @file EpicsMLDPConversion.h
 * @brief Conversion utilities for transforming PVXS values to MLDP protobuf DataFrame format.
 *
 * This header provides the EpicsMLDPConversion class which handles the conversion
 * of EPICS PVXS data types to the MLDP ingestion protobuf DataFrame columnar format.
 */

#pragma once

#include <ingestion.grpc.pb.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Utility class for converting PVXS values to MLDP protobuf DataFrame format.
 *
 * This class provides static methods to convert EPICS PV Access (PVXS) data structures
 * into typed DataFrame columns used by the MLDP ingestion service. The conversion
 * handles various EPICS data types including scalars, arrays, and structured types,
 * mapping each to the appropriate strongly-typed column (DoubleColumn, Int32Column, etc.).
 *
 * @note This class is designed for use with the PVXS library. For EPICS Base (pvData)
 *       conversions, see EpicsPVDataConversion.
 *
 * @see EpicsPVDataConversion for EPICS Base pvData conversions
 * @see BSASEpicsMLDPConversion for SLAC BSAS NTTable handling
 */
class EpicsMLDPConversion
{
public:
    /**
     * @brief Convert a PVXS value into a typed column appended to a DataFrame.
     *
     * Appends one typed column (or a set of sub-columns for compound types) to the
     * given DataFrame. Scalar PVXS types produce a single-element column; array types
     * produce a multi-element column; compound (Struct/Union/Any) types are expanded
     * recursively into sub-columns named @p columnName.fieldName.
     *
     * Type mapping:
     * - Bool / BoolA        → BoolColumn
     * - Int8/16/32 / arrays → Int32Column
     * - Int64 / array       → Int64Column
     * - UInt8/16/32 / arrays→ Int32Column (reinterpreted as signed)
     * - UInt64 / array      → Int64Column (reinterpreted as signed)
     * - Float32 / array     → FloatColumn
     * - Float64 / array     → DoubleColumn
     * - String / array      → StringColumn
     * - Struct/Union/Any    → recursive sub-columns
     * - Null                → StringColumn with value "null"
     *
     * @param[in]  pvValue    The PVXS value to convert.
     * @param[out] frame      DataFrame to which the typed column(s) will be appended.
     *                        Must not be null.
     * @param[in]  columnName Name assigned to the appended column (default: "value").
     *
     * @pre @p frame must point to a valid, initialized DataFrame.
     * @post On return, @p frame contains at least one additional typed column.
     */
    static void convertPVToDataFrame(const pvxs::Value&              pvValue,
                                     dp::service::common::DataFrame* frame,
                                     const std::string&              columnName = "value");
};

} // namespace mldp_pvxs_driver::reader::impl::epics