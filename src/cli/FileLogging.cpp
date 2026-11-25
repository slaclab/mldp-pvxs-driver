//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <cli/FileLogging.h>

#include <SpdlogLogger.h>
#include <util/log/Logger.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <vector>

namespace mldp_pvxs_driver::cli {

std::string sanitizeLogFileName(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out;
}

LogRoute classifyLogger(std::string_view name)
{
    constexpr std::string_view kController = "controller.";
    constexpr std::string_view kReader     = "reader:";
    constexpr std::string_view kWriter     = "_writer:";

    if (name.substr(0, kController.size()) == kController)
        return {LogCategory::Controller,
                "controller-" + sanitizeLogFileName(name.substr(kController.size()))};

    if (name.substr(0, kReader.size()) == kReader)
        return {LogCategory::Reader, "reader-" + sanitizeLogFileName(name.substr(kReader.size()))};

    if (const auto pos = name.find(kWriter); pos != std::string_view::npos)
    {
        const auto type     = name.substr(0, pos);
        const auto instance = name.substr(pos + kWriter.size());
        return {LogCategory::Writer,
                "writer-" + sanitizeLogFileName(type) + "_" + sanitizeLogFileName(instance)};
    }

    if (name.find("_reader") != std::string_view::npos)
        return {LogCategory::Reader, "reader-" + sanitizeLogFileName(name)};

    return {};
}

namespace {

struct SinkRegistry
{
    std::mutex                                     mutex;
    std::map<std::string, spdlog::sink_ptr>        componentSinks;
    std::vector<spdlog::sink_ptr>                  baseSinks; // console + combined
    LoggingConfig                                  cfg;
    std::string                                    runTag;
    std::string                                    pattern;
    std::shared_ptr<LogCompressor>                 compressor;
    spdlog::level::level_enum                      level{spdlog::level::info};

    spdlog::sink_ptr fileSink(const std::string& dir, const std::string& stem)
    {
        auto sink = std::make_shared<RotatingGzipFileSink>(dir, stem, runTag, cfg.rotation,
                                                           compressor);
        sink->set_pattern(pattern);
        return sink;
    }

    bool wantsOwnFile(LogCategory c) const
    {
        switch (c)
        {
        case LogCategory::Reader: return cfg.perReaders;
        case LogCategory::Writer: return cfg.perWriters;
        case LogCategory::Controller: return cfg.perController;
        default: return false;
        }
    }
};

} // namespace

std::shared_ptr<LogCompressor> installFileLogging(const LoggingConfig& cfg, const std::string& pattern)
{
    auto reg        = std::make_shared<SinkRegistry>();
    reg->cfg        = cfg;
    reg->pattern    = pattern;
    reg->runTag     = cfg.perRun
                          ? std::format("{:%Y%m%d-%H%M%S}",
                                        std::chrono::floor<std::chrono::seconds>(
                                            std::chrono::system_clock::now()))
                          : "";
    reg->compressor = std::make_shared<LogCompressor>();

    auto old = spdlog::default_logger();
    if (old)
    {
        reg->level = old->level();
        if (cfg.console)
            reg->baseSinks = old->sinks();
    }
    if (!cfg.logFile.empty())
        reg->baseSinks.push_back(reg->fileSink(cfg.logFileDirectory(), cfg.logFileStem()));

    const std::string defaultName = old ? old->name() : std::string("mldp_pvxs_driver");
    spdlog::drop_all();
    auto def = std::make_shared<spdlog::logger>(defaultName, reg->baseSinks.begin(),
                                                reg->baseSinks.end());
    def->set_level(reg->level);
    spdlog::set_default_logger(def);
    util::log::setLogger(std::make_shared<SpdlogLogger>(def));

    util::log::setLoggerFactory(
        [reg](std::string_view name) -> std::shared_ptr<util::log::ILogger>
        {
            const std::string loggerName{name};
            if (loggerName.empty())
                return util::log::getLoggerShared();

            std::lock_guard<std::mutex> lk(reg->mutex);
            if (auto existing = spdlog::get(loggerName))
                return std::make_shared<SpdlogLogger>(existing);

            std::vector<spdlog::sink_ptr> sinks = reg->baseSinks;
            const auto                    route = classifyLogger(loggerName);
            if (reg->wantsOwnFile(route.category))
            {
                auto& sink = reg->componentSinks[route.fileStem];
                if (!sink)
                    sink = reg->fileSink(reg->cfg.logFileDirectory(), route.fileStem);
                sinks.push_back(sink);
            }

            auto created = std::make_shared<spdlog::logger>(loggerName, sinks.begin(), sinks.end());
            created->set_level(reg->level);
            spdlog::register_logger(created);
            return std::make_shared<SpdlogLogger>(created);
        });

    spdlog::flush_every(std::chrono::seconds(2));
    return reg->compressor;
}

} // namespace mldp_pvxs_driver::cli
