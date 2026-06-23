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

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::config {

struct ConfigOverride
{
    std::string path;
    std::string value;
};

class ConfigOverrideError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] ConfigOverride parseConfigOverride(std::string_view arg);
void applyConfigOverride(Config& cfg, const ConfigOverride& overrideSpec);
void applyConfigOverrides(Config& cfg, const std::vector<std::string>& rawOverrides);
void applyConfigAssignment(Config& cfg, const ConfigOverride& overrideSpec);

} // namespace mldp_pvxs_driver::config
