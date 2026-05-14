//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#ifdef MLDP_WIZARD_ENABLED

#include <config/edit.h>
#include <config/Config.h>
#include <config/validate.h>
#include <config/wizard.h>
#include "wizard_internal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace mldp_pvxs_driver::config {

// Shared helpers (also defined in edit.cpp — redeclare as static here to avoid ODR issues
// by putting the shared logic in a thin internal helper).

static bool probeFileInteractive(const std::string& path)
{
    std::ifstream f{path};
    if (!f) { std::cerr << "ERROR  cannot open '" << path << "'\n"; return false; }
    f.seekg(0, std::ios::end);
    if (f.tellg() == 0) { std::cerr << "ERROR  '" << path << "' is empty\n"; return false; }
    return true;
}

static bool writeConfigInteractive(const std::string& path, const WizardState& st,
                                   bool dry_run, bool no_backup)
{
    const std::string yaml = wizard_internal::generateYaml(st);
    if (dry_run) { std::cout << yaml; return true; }
    if (!no_backup) {
        try {
            std::filesystem::copy_file(path, path + ".bak",
                std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            std::cerr << "WARN   backup failed: " << e.what() << "\n";
        }
    }
    std::ofstream out{path};
    if (!out) { std::cerr << "ERROR  cannot write '" << path << "'\n"; return false; }
    out << yaml;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

int runAddInteractive(const std::string& path, const std::string& kind,
                      bool no_backup, bool dry_run)
{
    if (!probeFileInteractive(path)) return 1;

    WizardState st;
    try {
        wizard_internal::loadFromConfig(path, st);
    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << "\n";
        return 1;
    }

    // Delegate entirely to the panel wizard; it handles add/save/validate.
    return runWizard(path, path);
}

} // namespace mldp_pvxs_driver::config

#endif // MLDP_WIZARD_ENABLED
