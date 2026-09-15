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
bool isPositiveDouble(const std::string& s);
bool isNonNegDouble(const std::string& s);

} // namespace mldp_pvxs_driver::config::wizard_internal
