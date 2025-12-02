#pragma once

#include <config/Config.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

/**
 * @brief Strongly typed view over an epics reader entry.
 */
class EpicsReaderConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    EpicsReaderConfig() = default;
    explicit EpicsReaderConfig(const Config& readerEntry);

    bool valid() const { return valid_; }
    const std::string& name() const { return name_; }
    const std::vector<std::string>& pvNames() const { return pvNames_; }

private:
    void parse(const Config& readerEntry);
    static std::vector<std::string> readStringSequence(const c4::yml::ConstNodeRef& node);

    bool                    valid_ = false;
    std::string             name_;
    std::vector<std::string> pvNames_;
};

} // namespace mldp_pvxs_driver::config
