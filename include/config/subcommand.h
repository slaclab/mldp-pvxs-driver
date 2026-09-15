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

namespace mldp_pvxs_driver::config {

/**
 * @brief Handle the "config" top-level subcommand.
 *
 * Called from main() when argv[1] == "config".
 * Parses argc/argv for sub-subcommands: wizard, validate, template.
 * Returns exit code (0 = success, 1 = error/invalid).
 */
int runConfigSubcommand(int argc, char** argv);

} // namespace mldp_pvxs_driver::config
