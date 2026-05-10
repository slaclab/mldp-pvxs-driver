//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// Internal API for testing — not part of the public install interface.
#pragma once

#include <config/wizard.h>
#include <functional>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config::wizard_internal {

// Pure YAML generation from a fully-populated WizardState.
std::string generateYaml(const WizardState& st);

// Populate WizardState from an existing YAML file (amend / --from mode).
void loadFromConfig(const std::string& path, WizardState& st);

// Field validators (return "" on valid, else error message).
bool isValidIso8601(const std::string& s);
bool isPositiveInt(const std::string& s);
bool isNonNegInt(const std::string& s);

#ifdef MLDP_WIZARD_ENABLED
// Single-entry interactive add sub-flows (called by `config add` interactive path).
// Each appends exactly one new entry to st and returns.
// used_names must be pre-populated with all existing writer/reader names for uniqueness checks.
void phase2_add_one_writer(WizardState& st);
void phase3_add_one_reader(WizardState& st);
// writer_name + writer_type must match an existing writer in st.
void phase5_add_one_routing_entry(WizardState& st,
                                  const std::string& writer_name,
                                  const std::string& writer_type);
#endif

} // namespace mldp_pvxs_driver::config::wizard_internal

#ifdef MLDP_WIZARD_ENABLED
// Blocking FTXUI prompt helpers — defined in wizard.cpp, in the config namespace.
namespace mldp_pvxs_driver::config {

std::string promptInput(const std::string& phase_title,
                        int phase, int total,
                        const std::string& field_label,
                        const std::string& def_val,
                        std::function<std::string(const std::string&)> validator = {});
bool promptYesNo(const std::string& phase_title,
                 int phase, int total,
                 const std::string& question,
                 bool default_yes);
int  promptMenu(const std::string& phase_title,
                int phase, int total,
                const std::string& question,
                const std::vector<std::string>& choices,
                int default_idx = 0);

} // namespace mldp_pvxs_driver::config
#endif
