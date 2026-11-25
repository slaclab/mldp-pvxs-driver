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

#include <filesystem>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

/**
 * @brief Merge configuration files and dotted-path overrides into one Config.
 *
 * Also records the directory of the last merged configuration file as the
 * process-wide configuration base directory (see configBaseDir()).
 */
Config loadMergedConfigSources(const std::vector<std::string>& sources);

/**
 * @brief Set the directory used to resolve relative paths found in the configuration.
 */
void setConfigBaseDir(const std::filesystem::path& dir);

/**
 * @brief Directory used to resolve relative paths found in the configuration.
 *
 * Empty when no configuration file was loaded (relative paths then resolve
 * against the current working directory).
 */
std::filesystem::path configBaseDir();

} // namespace mldp_pvxs_driver::config
