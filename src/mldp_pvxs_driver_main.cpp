//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <argparse/argparse.hpp>
#include <cstdlib>
#include <spdlog/common.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <util/log/Logger.h>

#include <csignal>
#include <format>

#include <iostream>

#include <termios.h>
#include <unistd.h>

#include <algorithm>

#define RYML_SINGLE_HDR_DEFINE_NOW
#include <rapidyaml-0.10.0.hpp>

#include <config/Config.h>
#include <controller/MLDPPVXSController.h>
#include <mldp_pvxs_driver_version.h>

namespace {
spdlog::level::level_enum parse_log_level(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });

    if (value == "trace")
        return spdlog::level::trace;
    if (value == "debug")
        return spdlog::level::debug;
    if (value == "info")
        return spdlog::level::info;
    if (value == "warn" || value == "warning")
        return spdlog::level::warn;
    if (value == "error" || value == "err")
        return spdlog::level::err;
    if (value == "critical" || value == "fatal")
        return spdlog::level::critical;
    if (value == "off")
        return spdlog::level::off;

    throw std::runtime_error(std::format(
        "Invalid log level '{}' (expected: trace, debug, info, warn, error, critical, off)",
        value));
}

namespace {

    class SpdlogLogger final : public mldp_pvxs_driver::util::log::ILogger
    {
    public:
        explicit SpdlogLogger(std::shared_ptr<spdlog::logger> logger)
            : logger_(std::move(logger))
        {
        }

        bool shouldLog(mldp_pvxs_driver::util::log::Level level) const override
        {
            if (!logger_)
            {
                return false;
            }
            return logger_->should_log(toSpd(level));
        }

        void log(mldp_pvxs_driver::util::log::Level level, std::string_view message) override
        {
            if (!logger_)
            {
                return;
            }
            logger_->log(toSpd(level), "{}", message);
        }

    private:
        static spdlog::level::level_enum toSpd(mldp_pvxs_driver::util::log::Level level)
        {
            using L = mldp_pvxs_driver::util::log::Level;
            switch (level)
            {
            case L::Trace: return spdlog::level::trace;
            case L::Debug: return spdlog::level::debug;
            case L::Info: return spdlog::level::info;
            case L::Warn: return spdlog::level::warn;
            case L::Error: return spdlog::level::err;
            case L::Critical: return spdlog::level::critical;
            case L::Off: return spdlog::level::off;
            }
            return spdlog::level::info;
        }

        std::shared_ptr<spdlog::logger> logger_;
    };

} // namespace

void disable_tty_echoctl()
{
    if (!::isatty(STDIN_FILENO))
        return;

    termios t;
    if (::tcgetattr(STDIN_FILENO, &t) != 0)
        return;

    t.c_lflag &= ~ECHOCTL;
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &t);
}
} // namespace

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::controller;

// Global variables for signal handling
std::mutex              m;
std::condition_variable cv;
bool                    quit = false;
// Global driver instance
std::shared_ptr<MLDPPVXSController> driver = nullptr;

int main(int argc, char** argv)
{

    disable_tty_echoctl();

    argparse::ArgumentParser program(
        "MLDP PVXS Driver",
        std::format("{}.{}.{}", MLDP_PVXS_DRIVER_VERSION_MAJOR, MLDP_PVXS_DRIVER_VERSION_MINOR, MLDP_PVXS_DRIVER_VERSION_PATCH));
    program.add_argument("--config", "-c")
        .default_value("config.yaml")
        .help("Path to the configuration file")
        .nargs(1)
        .action([](const std::string& value)
                {
                    return value;
                });

    program.add_argument("--log-level", "-l")
        .default_value(spdlog::level::info)
        .help("Logging level (trace, debug, info, warn, error, critical, off)")
        .nargs(1)
        .action([](const std::string& value)
                {
                    return parse_log_level(value);
                });

    try
    {
        // Register signal handlers
        const auto exitHandler = [](int)
        {
            if (driver)
            {
                std::lock_guard lk(m);
                quit = true;
                cv.notify_one();
            }
        };
        std::signal(SIGINT, exitHandler);
        std::signal(SIGTERM, exitHandler);

        // Parse command line arguments
        program.parse_args(argc, argv);

        if (!spdlog::default_logger())
        {
            spdlog::set_default_logger(spdlog::stdout_color_mt("mldp_pvxs_driver"));
        }
        else if (spdlog::default_logger()->name().empty())
        {
            // Ensure pattern %n is meaningful for the process-wide default logger.
            spdlog::set_default_logger(spdlog::stdout_color_mt("mldp_pvxs_driver"));
        }
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%-32!n] [%^%-8l%$] %v");
        spdlog::set_level(program.get<spdlog::level::level_enum>("--log-level"));

        // Install the executable's spdlog logger as the driver library logger.
        // Also provide a factory for named loggers so library components can request
        // per-component/per-instance loggers without depending on spdlog.
        mldp_pvxs_driver::util::log::setLogger(std::make_shared<SpdlogLogger>(spdlog::default_logger()));
        mldp_pvxs_driver::util::log::setLoggerFactory([](std::string_view name) -> std::shared_ptr<mldp_pvxs_driver::util::log::ILogger>
                                                      {
                                                          const std::string loggerName{name};

                                                          if (loggerName.empty())
                                                          {
                                                              return mldp_pvxs_driver::util::log::getLoggerShared();
                                                          }

                                                          if (auto existing = spdlog::get(loggerName))
                                                          {
                                                              return std::static_pointer_cast<mldp_pvxs_driver::util::log::ILogger>(
                                                                  std::make_shared<SpdlogLogger>(existing));
                                                          }

                                                          auto                            base = spdlog::default_logger();
                                                          std::shared_ptr<spdlog::logger> created;
                                                          if (base)
                                                          {
                                                              created = base->clone(loggerName);
                                                          }
                                                          else
                                                          {
                                                              created = spdlog::stdout_color_mt(loggerName);
                                                          }

                                                          // Keep behavior similar to previous code: reuse by name.
                                                          spdlog::register_logger(created);
                                                          return std::static_pointer_cast<mldp_pvxs_driver::util::log::ILogger>(
                                                              std::make_shared<SpdlogLogger>(created));
                                                      });
        // Log version information
        spdlog::info(
            "MLDP PVXS Driver Version {}.{}.{}",
            MLDP_PVXS_DRIVER_VERSION_MAJOR,
            MLDP_PVXS_DRIVER_VERSION_MINOR,
            MLDP_PVXS_DRIVER_VERSION_PATCH);

        // Load configuration
        auto config_path = program.get<std::string>("--config");
        spdlog::info("Loading configuration from {}", config_path);
        auto config = Config::configFromFile(config_path);

        // Start the driver
        spdlog::info("Starting driver...");
        driver = MLDPPVXSController::create(config);
        driver->start();

        // Wait for shutdown
        std::unique_lock lk(m);
        cv.wait(lk,
                []
                {
                    return quit;
                });

        // Stop the driver
        spdlog::info("Stopping driver...");
        driver->stop();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& err)
    {
        // Keep user output clean: avoid C++ runtime "terminate called..." noise.
        std::cerr << "Error: " << err.what() << "\n";
        return EXIT_FAILURE;
    }
    catch (...)
    {
        std::cerr << "Error: Unknown failure\n";
        return EXIT_FAILURE;
    }
}
