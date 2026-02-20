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
 * @brief Conversion utilities for transforming PVXS values to MLDP protobuf format.
 *
 * This header provides the EpicsMLDPConversion class which handles the conversion
 * of EPICS PVXS data types to the MLDP ingestion protobuf DataValue format.
 */

#pragma once

#include <ingestion.grpc.pb.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Utility class for converting PVXS values to MLDP protobuf DataValue format.
 *
 * This class provides static methods to convert EPICS PV Access (PVXS) data structures
 * into the protobuf DataValue format used by the MLDP ingestion service. The conversion
 * handles various EPICS data types including scalars, arrays, and structured types.
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
     * @brief Convert a PVXS value to an MLDP protobuf DataValue.
     *
     * Transforms the given PVXS Value into the corresponding protobuf DataValue
     * representation. The method handles scalar types (int, double, string, etc.),
     * array types, and enum types.
     *
     * @param[in] pvValue The PVXS value to convert. Must be a valid value field
     *                    (typically extracted from a monitored PV structure).
     * @param[out] protoValue Pointer to the DataValue protobuf message that will
     *                        be populated with the converted data. Must not be null.
     *
     * @pre @p protoValue must point to a valid DataValue object.
     * @post @p protoValue contains the converted representation of @p pvValue.
     *
     * @note For compound types, only the "value" field is extracted and converted.
     *       Alarm and timestamp information should be handled separately.
     */
    static void convertPVToProtoValue(const pvxs::Value& pvValue, DataValue* protoValue);
};

} // namespace mldp_pvxs_driver::reader::impl::epics