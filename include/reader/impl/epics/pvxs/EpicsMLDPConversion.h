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
 * @brief Conversion utilities for transforming PVXS values to MLDP DataBatch format.
 *
 * This header provides the EpicsMLDPConversion class which handles the conversion
 * of EPICS PVXS data types to the MLDP bus DataBatch columnar format.
 */

#pragma once

#include <pvxs/client.h>
#include <pvxs/nt.h>
#include <string>
#include <util/bus/DataBatch.h>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Utility class for converting PVXS values to MLDP DataBatch format.
 *
 * This class provides static methods to convert EPICS PV Access (PVXS) data structures
 * into typed DataBatch columns used by the MLDP bus. The conversion handles various
 * EPICS data types including scalars, arrays, and structured types, mapping each to
 * the appropriate strongly-typed ColumnValues variant.
 *
 * Type mapping:
 * - Bool / BoolA          → std::vector<bool> / std::vector<std::vector<bool>>
 * - Int8/16/32 / arrays   → std::vector<int32_t> / std::vector<std::vector<int32_t>>
 * - Int64 / array         → std::vector<int64_t> / std::vector<std::vector<int64_t>>
 * - UInt8/16/32 / arrays  → std::vector<int32_t> / std::vector<std::vector<int32_t>> (reinterpreted)
 * - UInt64 / array        → std::vector<int64_t> / std::vector<std::vector<int64_t>> (reinterpreted)
 * - Float32 / array       → std::vector<float> / std::vector<std::vector<float>>
 * - Float64 / array       → std::vector<double> / std::vector<std::vector<double>>
 * - String / StringA      → std::vector<std::string> (array dims recorded for StringA)
 * - Struct/Union/Any      → recursive sub-columns with dotted names
 * - Null                  → std::vector<std::string>{"null"}
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
     * @brief Convert a PVXS value into typed column(s) appended to a DataBatch.
     *
     * Appends one DataColumn (or a set of sub-columns for compound types) to the
     * given DataBatch. Scalar PVXS types produce a single-element column; array
     * types produce a vector-of-vectors column (one inner vector per sample) and
     * record the array shape in @p batch->array_dims; compound (Struct/Union/Any)
     * types are expanded recursively into sub-columns named @p columnName.fieldName.
     *
     * @param[in]  pvValue    The PVXS value to convert.
     * @param[out] batch      DataBatch to which the typed column(s) will be appended.
     *                        Must not be null. The caller is responsible for setting
     *                        timestamps before or after this call.
     * @param[in]  columnName Name assigned to the appended column (default: "value").
     *
     * @pre @p batch must point to a valid, initialized DataBatch.
     * @post On return, @p batch contains at least one additional DataColumn.
     */
    static void convertPVToDataBatch(const pvxs::Value&        pvValue,
                                     util::bus::DataBatch*     batch,
                                     const std::string&        columnName = "value");
};

} // namespace mldp_pvxs_driver::reader::impl::epics
