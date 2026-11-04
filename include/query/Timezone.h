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

/** @brief Formats a UTC Arrow timestamp in the client's local timezone (one-argument `from_utc`).
 *
 * Uses the `TZ` environment variable when set (IANA name or +/-HH:MM, leading ':' stripped), else the
 * host's IANA zone. When the host zone has no resolvable name (e.g. `/etc/localtime` is a copied file
 * rather than a symlink, common in containers) the C library's local time is used per instant, so
 * daylight saving time is still applied.
 * @param[in] timestamp UTC Arrow timestamp scalar.
 * @return Formatted local time string with its UTC offset. */
[[nodiscard]] std::string fromUtcLocal(const arrow::TimestampScalar& timestamp);

} // namespace mldp_pvxs_driver::query
