#pragma once

#include <config/Config.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

/**
 * @brief Strongly typed view over the `reader` section of the driver YAML.
 */
class ReaderConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    ReaderConfig() = default;
    explicit ReaderConfig(const Config& root);

    bool valid() const { return valid_; }
    const std::string& type() const { return type_; }
    const std::vector<std::string>& pvNames() const { return pvNames_; }

private:
    void parseReaderNode(const Config& readerNode);
    static std::vector<std::string> readStringSequence(const c4::yml::ConstNodeRef& node);

    bool                    valid_ = false;
    std::string             type_;
    std::vector<std::string> pvNames_;
};

} // namespace mldp_pvxs_driver::config
