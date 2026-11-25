//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <cli/LoggingConfig.h>

#include <filesystem>
#include <stdexcept>

namespace mldp_pvxs_driver::cli {

namespace {

int nonNegative(const config::Config& node, const std::string& key, int def)
{
    const int v = node.getInt(key, def);
    if (v < 0)
        throw std::runtime_error("logging: '" + key + "' must be >= 0");
    return v;
}

} // namespace

LoggingConfig LoggingConfig::parse(const config::Config& root, const std::string& logFileOverride)
{
    LoggingConfig cfg = parseBlock(root);
    if (!logFileOverride.empty())
        cfg.logFile = logFileOverride;
    cfg.validate();
    return cfg;
}

LoggingConfig LoggingConfig::parseBlock(const config::Config& root)
{
    LoggingConfig cfg;
    if (!root.hasChild("logging"))
        return cfg;

    // hasChild() guarantees subConfig() returns at least one element.
    const auto node = root.subConfig("logging").front();

    if (node.hasChild("combined-file"))
        throw std::runtime_error("logging: 'combined-file' was replaced by 'log-file: <path>'");
    if (node.hasChild("directory"))
        throw std::runtime_error(
            "logging: 'directory' is not supported; per-component files are written next to 'log-file'");

    cfg.logFile   = node.get("log-file", "");
    cfg.console   = node.getBool("console", true);
    cfg.perRun    = node.getBool("per-run", true);

    if (node.hasChild("per-component"))
    {
        const auto pc = node.subConfig("per-component");
        if (!pc.empty())
        {
            cfg.perReaders    = pc.front().getBool("readers", false);
            cfg.perWriters    = pc.front().getBool("writers", false);
            cfg.perController = pc.front().getBool("controller", false);
        }
    }

    if (node.hasChild("rotation"))
    {
        const auto rot = node.subConfig("rotation");
        if (!rot.empty())
        {
            const auto& r = rot.front();
            cfg.rotation.maxBytes =
                static_cast<std::uint64_t>(nonNegative(r, "max-size-mb", 100)) * 1024ULL * 1024ULL;
            cfg.rotation.maxAge   = std::chrono::hours(nonNegative(r, "max-age-hours", 0));
            cfg.rotation.compress = r.getBool("compress", true);
            cfg.rotation.maxFiles = nonNegative(r, "max-files", 10);
        }
    }

    return cfg;
}

void LoggingConfig::validate() const
{
    if (anyPerComponent() && logFile.empty())
        throw std::runtime_error(
            "logging: per-component files need 'log-file' (they are written in its folder)");
    if (!logFile.empty() && std::filesystem::path(logFile).filename().empty())
        throw std::runtime_error("logging: 'log-file' must be a file path, got: " + logFile);
}

std::string LoggingConfig::logFileDirectory() const
{
    const auto parent = std::filesystem::path(logFile).parent_path().string();
    return parent.empty() ? std::string(".") : parent;
}

std::string LoggingConfig::logFileStem() const
{
    std::string name = std::filesystem::path(logFile).filename().string();
    constexpr std::string_view kExt = ".log";
    if (name.size() > kExt.size() && name.compare(name.size() - kExt.size(), kExt.size(), kExt) == 0)
        name.resize(name.size() - kExt.size());
    return name;
}

} // namespace mldp_pvxs_driver::cli
