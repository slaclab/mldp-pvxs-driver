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

namespace mldp_pvxs_driver::cli {

/**
 * @brief Build a compact, user-friendly startup summary of the effective config.
 *
 * The output is designed for terminal readability and intentionally keeps
 * per-reader details on one line with abbreviated PV previews.
 */
std::string formatStartupConfig(const mldp_pvxs_driver::config::Config& root,
                                const std::string&                      configPath);

/**
 * @brief Flatten YAML config into a compact table: `key.path | value`.
 *
 * Useful as an error-safe fallback when effective typed formatting fails but the
 * YAML has been parsed.
 */
std::string formatConfigKeyValueTable(const mldp_pvxs_driver::config::Config& root,
                                      const std::string&                      configPath);

} // namespace mldp_pvxs_driver::cli
