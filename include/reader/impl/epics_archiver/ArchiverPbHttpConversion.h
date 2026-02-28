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
 * @file ArchiverPbHttpConversion.h
 * @brief Type-conversion utilities for Archiver Appliance PB/HTTP payload types.
 *
 * Provides a static-only utility class that parses serialised protobuf samples
 * from the EPICS Archiver Appliance PB/HTTP protocol into the internal
 * EventValue format used by the event bus.  All 15 payload types defined in
 * EPICSEvent.proto are supported.
 */

#pragma once

#include <EPICSEvent.pb.h>
#include <util/bus/IDataBus.h>

#include <cstdint>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {

/**
 * @brief A fully parsed archiver sample ready for event-bus ingestion.
 */
struct ParsedSample
{
    uint64_t                              epoch_seconds; ///< UNIX epoch seconds of the sample.
    uint32_t                              nanoseconds;   ///< Sub-second nanoseconds of the sample.
    util::bus::IDataBus::EventValue  event;         ///< Converted event value (timestamp + DataValue).
};

/**
 * @brief Static conversion utilities for Archiver Appliance PB/HTTP samples.
 *
 * Parses one raw serialised protobuf sample (already PB/HTTP-unescaped) for
 * any of the 15 payload types defined in EPICSEvent.proto and converts it into
 * a @ref ParsedSample.
 *
 * @note This class is not instantiable; all methods are static.
 */
class ArchiverPbHttpConversion
{
public:
    ArchiverPbHttpConversion() = delete;

    /**
     * @brief Parse a single serialised archiver sample into a ParsedSample.
     *
     * @param header    The PayloadInfo header for the current PB/HTTP chunk.
     *                  Supplies the payload type and the year needed to compute
     *                  the UNIX epoch timestamp.
     * @param msg_bytes Raw serialised bytes for the sample proto message
     *                  (already PB/HTTP-unescaped).
     *
     * @return A fully populated ParsedSample with epoch_seconds, nanoseconds,
     *         and a typed DataValue.
     *
     * @throws std::runtime_error if the proto message cannot be parsed, or if
     *         the payload type is not one of the 15 supported types.
     */
    static ParsedSample parseSample(const EPICS::PayloadInfo& header,
                                    const std::string&        msg_bytes);
};

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
