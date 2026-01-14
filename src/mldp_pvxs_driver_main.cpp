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
#include <atomic>
#include <cstdlib>
#include <memory>
#include <spdlog/common.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <util/log/Logger.h>

#include <csignal>
#include <format>

#include <iostream>
#include <sstream>

#include <termios.h>
#include <unistd.h>

#include <poll.h>

#include <string_view>

#define RYML_SINGLE_HDR_DEFINE_NOW
#include <prometheus/text_serializer.h>
#include <rapidyaml-0.10.0.hpp>

#include <config/Config.h>
#include <controller/MLDPPVXSController.h>
#include <metrics/MetricsSnapshot.h>
#include <mldp_pvxs_driver_version.h>

#include "PeriodicMetricsDumper.h"
#include "SpdlogLogger.h"

using namespace argparse;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::controller;
using mldp_pvxs_driver::cli::SpdlogLogger;

namespace {

// Disable echoing of control characters like ^C in the terminal.
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

// State for restoring terminal settings on exit.
struct TerminalRestoreState
{
    termios old{};
    bool    active{false};
    bool    registered{false};
};

TerminalRestoreState g_terminal_restore;

// Restore terminal settings if previously modified.
void restore_terminal()
{
    if (!g_terminal_restore.active)
    {
        return;
    }
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &g_terminal_restore.old);
    g_terminal_restore.active = false;
}

// Configure command line argument parser.
void configure_parameter(ArgumentParser& program)
{
    program.add_description("MLDP PVXS Driver - Forwards reader updates (e.g., EPICS PVs) to the MLDP ingestion API. Supports multiple reader implementations.");
    program.add_argument("-c", "--config")
        .help("Path to configuration YAML file")
        .default_value(std::string("config.yaml"))
        .metavar("FILE");

    program.add_argument("-l", "--log-level")
        .help("Logging level (trace, debug, info, warn, error, critical, off)")
        .default_value(std::string("info"))
        .metavar("LEVEL");

    program.add_argument("-m", "--metrics-output")
        .help("Path to output file for periodic metrics dumps (JSON Lines format)")
        .default_value(std::string("metrics.jsonl"))
        .metavar("FILE");

    program.add_argument("--metrics-interval")
        .help("Interval in seconds for periodic metrics dumps (default: 60)")
        .default_value(5)
        .scan<'d', int>()
        .metavar("SECONDS");

    // add metrics help to epilog
    program.add_epilog(
        R"(Metrics:

     - Press Ctrl+P in the foreground terminal to dump metrics.
     - Or send SIGUSR1 / SIGQUIT to request a dump:
        kill -USR1 <pid>
        kill -QUIT <pid>
     - Use --metrics-output to specify a file path for periodic metric dumps
       (stored in JSON Lines format with configurable interval).
    )");
}

// Guard used to set terminal to cbreak mode.
// Restores terminal settings on destruction.
struct TermCbreakGuard
{
    termios old{};
    bool    active{false};

    TermCbreakGuard()
    {
        if (!::isatty(STDIN_FILENO))
        {
            return;
        }

        termios current{};
        if (::tcgetattr(STDIN_FILENO, &current) != 0)
        {
            return;
        }

        old = current;
        termios t = current;
        t.c_lflag &= ~(ICANON); // read byte-by-byte
        t.c_lflag &= ~(ECHO);   // optional: don't echo typed keys
        t.c_lflag |= ISIG;      // keep Ctrl+C => SIGINT
        t.c_lflag &= ~ECHOCTL;  // optional: don't show ^C when ECHO is on

        if (::tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
        {
            active = true;

            // Ensure restoration also happens on std::exit() (e.g. argparse --help).
            g_terminal_restore.old = old;
            g_terminal_restore.active = true;
            if (!g_terminal_restore.registered)
            {
                g_terminal_restore.registered = true;
                std::atexit(restore_terminal);
            }
        }
    }

    ~TermCbreakGuard()
    {
        if (!active)
        {
            return;
        }
        restore_terminal();
    }
};

// Serialize metrics to Prometheus text exposition format.
std::string serializeMetricsText(const mldp_pvxs_driver::metrics::Metrics& metrics)
{
    prometheus::TextSerializer serializer;
    std::ostringstream         out;
    serializer.Serialize(out, metrics.registry()->Collect());
    return out.str();
}

} // namespace

// Global flags for signal handling
volatile std::atomic<bool> quit = false;
volatile std::atomic<bool> metrics_requested = false;

// Global driver instance
std::shared_ptr<MLDPPVXSController>    driver = nullptr;
std::shared_ptr<PeriodicMetricsDumper> metrics_dumper = nullptr;

int main(int argc, char** argv)
{
    // Register signal handlers early so SIGINT/SIGTERM won't terminate the
    // process before we can restore terminal settings.
    const auto exitHandler = [](int)
    {
        quit = true;
    };
    std::signal(SIGINT, exitHandler);
    std::signal(SIGTERM, exitHandler);

    const auto metricsSignalHandler = [](int)
    {
        metrics_requested = true;
    };
    std::signal(SIGUSR1, metricsSignalHandler);
    std::signal(SIGQUIT, metricsSignalHandler);

    // Configure command line argument parser
    ArgumentParser program(
        "MLDP PVXS Driver",
        std::format("{}.{}.{}", MLDP_PVXS_DRIVER_VERSION_MAJOR, MLDP_PVXS_DRIVER_VERSION_MINOR, MLDP_PVXS_DRIVER_VERSION_PATCH));

    configure_parameter(program);

    const auto start_dumper = [&program](std::shared_ptr<PeriodicMetricsDumper>& metrics_dumper)
    {
        if (metrics_dumper)
        {
            spdlog::warn("Periodic metrics dumper is already running.");
            return;
        }
        // Ctrl+D
        const auto interval_seconds = program.get<int>("--metrics-interval");
        spdlog::info(
            "Starting periodic metrics dumper to file '{}' every {} seconds...",
            program.get<std::string>("--metrics-output"),
            interval_seconds);
        metrics_dumper = std::make_shared<PeriodicMetricsDumper>(
            driver->metrics(),
            program.get<std::string>("--metrics-output"),
            std::chrono::milliseconds(interval_seconds * 1000));
    };

    const auto stop_dumper = [&program](std::shared_ptr<PeriodicMetricsDumper>& metrics_dumper)
    {
        // Stop periodic metrics dumper if it was started
        if (metrics_dumper)
        {
            spdlog::info("Stopping periodic metrics dumper...");
            metrics_dumper.reset();
            spdlog::info("Stopped periodic metrics dumper.");
        }
    };

    try
    {
        // Parse command line arguments
        program.parse_args(argc, argv);

        // Set terminal to cbreak mode (no-echo) only after successful arg parsing.
        TermCbreakGuard termGuard;
        // Metrics printing can be triggered either by Ctrl+P (foreground terminal)
        // or by sending SIGUSR1/SIGQUIT to the process.
        mldp_pvxs_driver::metrics::MetricsSnapshot metrics_snapshot;
        const auto metricHandler = [&metrics_snapshot]()
        {
            if (driver)
            {
                const auto snapshot = metrics_snapshot.getSnapshot(driver->metrics());
                std::cout << mldp_pvxs_driver::metrics::MetricsSnapshot::toString(snapshot);
                std::cout.flush();
            }
        };

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

        // Install the executable's spdlog logger as the driver library logger.
        // Also provide a factory for named loggers so library components can request
        // per-component/per-instance loggers without depending on spdlog.
        auto logger = std::make_shared<SpdlogLogger>(spdlog::default_logger());
        logger->setLevel(program.get<std::string>("--log-level"));

        setLogger(logger);
        setLoggerFactory([](std::string_view name) -> std::shared_ptr<ILogger>
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
        pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        while (!quit)
        {
            if (metrics_requested)
            {
                metrics_requested = false;
                metricHandler();
            }

            // Poll for keyboard input with timeout so signals can stop the loop quickly.
            constexpr int timeout_ms = 100;
            const int     rc = ::poll(&pfd, 1, timeout_ms);
            if (rc <= 0)
            {
                continue;
            }
            if ((pfd.revents & POLLIN) == 0)
            {
                continue;
            }

            unsigned char c = 0;
            const auto    n = ::read(STDIN_FILENO, &c, 1);
            if (n != 1)
            {
                continue;
            }
            if (c == ('p' & 0x1F))
            {
                // Ctrl+P
                metricHandler();
            }

            // metric dumper activations
            if (c == ('d' & 0x1F))
            {
                if (metrics_dumper)
                {
                    stop_dumper(metrics_dumper);
                }
                else
                {
                    start_dumper(metrics_dumper);
                }
            }
        }

        // Stop the driver
        spdlog::info("Stopping driver...");
        driver->stop();

        // Stop periodic metrics dumper if it was started
        stop_dumper(metrics_dumper);

        // Ensure all driver-owned components (and their loggers) are destroyed
        // before leaving main(), to avoid static-destruction-order issues.
        driver.reset();
        setLogger(nullptr);
        setLoggerFactory({});
        spdlog::shutdown();
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
