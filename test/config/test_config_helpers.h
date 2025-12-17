// Helper utilities shared by configuration tests.
#pragma once

#include <config/Config.h>

namespace mldp_pvxs_driver::config {

inline Config makeConfigFromYaml(const std::string& yaml)
{
    // Use parse_in_arena so the tree owns its buffer and remains valid for tests.
    auto tree = std::make_shared<ryml::Tree>(ryml::parse_in_arena(c4::to_csubstr(yaml)));
    return Config(tree);
}

} // namespace mldp_pvxs_driver::config
