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
        std::cerr << "ERROR  failed to parse '" << path << "': " << e.what() << "\n";
        return 1;
    }

    if (kind == "writer") {
        wizard_internal::phase2_add_one_writer(st);

    } else if (kind == "reader") {
        wizard_internal::phase3_add_one_reader(st);

    } else if (kind == "routing") {
        // Ask which writer to configure routing for
        std::vector<std::string> writer_names;
        std::vector<std::string> writer_types;
        for (const auto& w : st.mldp_writers) {
            writer_names.push_back(w.name);
            writer_types.push_back("mldp");
        }
        for (const auto& w : st.hdf5_writers) {
            writer_names.push_back(w.name);
            writer_types.push_back(w.is_merge ? "hdf5-merge" : "hdf5");
        }
        if (writer_names.empty()) {
            std::cerr << "ERROR  no writers in config — add a writer first\n";
            return 1;
        }
        std::cout << "Writers available:\n";
        for (std::size_t i = 0; i < writer_names.size(); ++i)
            std::cout << "  [" << i << "] " << writer_names[i] << " (" << writer_types[i] << ")\n";
        std::cout << "Select writer index: ";
        int idx = 0;
        std::cin >> idx;
        if (idx < 0 || static_cast<std::size_t>(idx) >= writer_names.size()) {
            std::cerr << "ERROR  invalid index\n";
            return 1;
        }
        if (!st.routing_all_to_all) {
            st.routing_all_to_all = false;
        } else {
            st.routing_all_to_all = false;
            std::cerr << "WARN   switching from all-to-all to explicit routing; "
                         "writers not listed will receive nothing\n";
        }
        wizard_internal::phase5_add_one_routing_entry(st, writer_names[idx], writer_types[idx]);

    } else {
        std::cerr << "ERROR  unknown kind '" << kind << "' — use writer, reader, or routing\n";
        return 1;
    }

    // Validate
    const std::string yaml = wizard_internal::generateYaml(st);
    auto tree = std::make_shared<ryml::Tree>(ryml::parse_in_arena(c4::to_csubstr(yaml)));
    Config cfg(tree);
    const auto diags = validateConfig(cfg);
    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == ConfigDiagnostic::Severity::ERROR) {
            std::cerr << "ERROR  " << d.field_path << "  " << d.message << "\n";
            has_error = true;
        } else {
            std::cerr << "WARN   " << d.field_path << "  " << d.message << "\n";
        }
    }
    if (has_error) {
        std::cerr << "FAIL  result would be invalid — not writing\n";
        return 1;
    }

    // Confirm save
    std::cout << "Save to '" << path << "'? [y/N] ";
    std::string ans;
    std::cin >> ans;
    if (ans != "y" && ans != "Y") {
        std::cout << "Aborted.\n";
        return 0;
    }

    if (!writeConfigInteractive(path, st, dry_run, no_backup)) return 1;

    if (!dry_run) {
        std::cout << "Saved to " << path << "\n";
        if (!no_backup) std::cout << "Backup: " << path << ".bak\n";
    }
    return 0;
}

} // namespace mldp_pvxs_driver::config

#endif // MLDP_WIZARD_ENABLED
