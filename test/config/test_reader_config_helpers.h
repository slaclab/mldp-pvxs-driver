#include <config/ReaderConfig.h>
namespace mldp_pvxs_driver::config {
    // Helper function to create Config from YAML string for testing
    inline Config makeConfigFromYaml(const std::string& yaml)
    {
        // rapidyaml requires a mutable buffer for parse_in_place
        auto buffer = std::make_shared<std::string>(yaml);
        auto tree = std::make_shared<ryml::Tree>(ryml::parse_in_place(c4::to_substr(*buffer)));
        return Config(tree);
    }


} // namespace mldp_pvxs_driver::config