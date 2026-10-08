//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file Timezone.h
 * @brief Declares timezone parsing and UTC timestamp projection helpers. */
#pragma once

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <string>

namespace mldp_pvxs_driver::query {

/** @brief Formats a UTC Arrow timestamp as a local time string in the given IANA timezone or fixed UTC offset.
 * @param[in] timestamp      UTC Arrow timestamp scalar.
 * @param[in] zone_or_offset IANA timezone name (e.g. "America/Los_Angeles") or fixed offset (e.g. "+05:30").
 * @return Formatted local time string.
 * @throws std::runtime_error  If the timezone is unknown or the conversion fails. */
[[nodiscard]] std::string fromUtc(const arrow::TimestampScalar& timestamp, const std::string& zone_or_offset);

/** @brief Returns the client's local timezone, used by the one-argument `from_utc(timestamp)`.
 * @return The `TZ` environment variable when set (IANA name or +/-HH:MM, leading ':' stripped),
 *         otherwise the host's IANA timezone, or "+00:00" when it cannot be determined. */
[[nodiscard]] std::string localTimezone();

} // namespace mldp_pvxs_driver::query
