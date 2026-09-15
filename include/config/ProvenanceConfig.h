//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//!
//! \file
//! \brief Utility for parsing provenance metadata from reader configuration.
//!
//! Provides a free function that extracts the `provenance:` map from a
//! Config node, returning it as an unordered_map suitable for attaching
//! to events for data lineage tracking.
#pragma once

#include <config/Config.h>

#include <map>
#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::config {

/**
 * @brief Parse the `provenance:` block from a reader config entry.
 *
 * Looks for a child node named "provenance" in @p cfg. If present, parses
 * it as a string-to-string map and returns the entries. If absent, returns
 * an empty map.
 *
 * Expected YAML structure:
 * @code
 * reader:
 *   epics-pvxs:
 *     - name: reader_name
 *       provenance:
 *         facility: LCLS
 *         instrument: CXI
 *         subsystem: BSAS
 * @endcode
 *
 * @param cfg The Config node representing a single reader entry.
 * @return Parsed provenance key-value pairs, or empty map if not configured.
 */
inline std::unordered_map<std::string, std::string> parseProvenance(const Config& cfg)
{
    std::unordered_map<std::string, std::string> result;
    if (cfg.hasChild("provenance"))
    {
        std::map<std::string, std::string> m;
        cfg.subConfig("provenance").front() >> m;
        result.insert(m.begin(), m.end());
    }
    return result;
}

} // namespace mldp_pvxs_driver::config
