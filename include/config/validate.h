//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <config/Config.h>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

struct ConfigDiagnostic
{
    enum class Severity
    {
        ERROR,
        WARN
    };
    Severity    severity;
    std::string field_path; // e.g. "writer.mldp[0].mldp-pool.ingestion-url"
    std::string message;    // human-readable description
};

/**
 * @brief Validate a fully-parsed Config against all semantic rules.
 *
 * Returns a list of diagnostics (errors and warnings).
 * An empty list means the config is valid.
 * Does NOT throw — all issues are returned as diagnostics.
 */
std::vector<ConfigDiagnostic> validateConfig(const Config& cfg);

} // namespace mldp_pvxs_driver::config
