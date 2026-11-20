//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file   mldppvxs_controller_file_logging_integration_test.cpp
 * @brief  End-to-end file logging with a real controller, reader and writer.
 *
 * Pipeline: YAML (incl. `logging:`) → LoggingConfig::parse → installFileLogging
 * → MLDPPVXSController (slac-calendar reader against MockCalendarHttpServer,
 * mldp-configuration writer against an in-process fake annotation gRPC service).
 * Checks which file each component's lines land in, rotation/compression, and
 * that the default (no per-component) setup writes only the main file.
 */

#include <gtest/gtest.h>

#include <annotation.grpc.pb.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <cli/FileLogging.h>
#include <cli/LoggingConfig.h>
#include <controller/MLDPPVXSController.h>
#include <util/log/Logger.h>

#include <spdlog/spdlog.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../config/test_config_helpers.h"
#include "../mock/MockCalendarHttpServer.h"

using mldp_pvxs_driver::cli::installFileLogging;
using mldp_pvxs_driver::cli::LogCompressor;
using mldp_pvxs_driver::cli::LoggingConfig;
using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::test::mock::MockCalendarHttpServer;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Fake annotation service (answers every save with a success result)
// ---------------------------------------------------------------------------

class LoggingTestAnnotationSvc final : public dp::service::annotation::DpAnnotationService::Service
{
public:
    std::atomic<int> configs{0};
    std::atomic<int> activations{0};

    grpc::Status saveConfiguration(grpc::ServerContext*,
                                   const dp::service::annotation::SaveConfigurationRequest* req,
                                   dp::service::annotation::SaveConfigurationResponse* resp) override
    {
        resp->mutable_saveconfigurationresult()->set_configurationname(req->configurationname());
        configs.fetch_add(1);
        return grpc::Status::OK;
    }

    grpc::Status saveConfigurationActivation(
        grpc::ServerContext*,
        const dp::service::annotation::SaveConfigurationActivationRequest* req,
        dp::service::annotation::SaveConfigurationActivationResponse* resp) override
    {
        resp->mutable_saveconfigurationactivationresult()->set_clientactivationid(
            req->clientactivationid());
        activations.fetch_add(1);
        return grpc::Status::OK;
    }
};

/// Two valid events + one with an empty program_name (forces a reader warning).
const char* kEvents = R"json([
  {
    "url": "https://www.google.com/calendar/event?eid=log1",
    "program_name": "CXI Logging Run",
    "description": "first",
    "calendar": "NC-CXI",
    "start": "2026-05-28T06:00:00-07:00",
    "end":   "2026-05-28T18:00:00-07:00",
    "hutch": {"name": "CXI"},
    "machine": "NC"
  },
  {
    "url": "https://www.google.com/calendar/event?eid=log2",
    "program_name": "MEC Logging Run",
    "description": "second",
    "calendar": "NC-MEC",
    "start": "2026-05-29T06:00:00-07:00",
    "end":   "2026-05-29T18:00:00-07:00",
    "hutch": {"name": "MEC"},
    "machine": "NC"
  },
  {
    "url": "https://www.google.com/calendar/event?eid=nameless",
    "program_name": "",
    "description": "no program name here",
    "calendar": "NC-XPP",
    "start": "2026-05-30T08:00:00-07:00",
    "end":   "2026-05-30T09:00:00-07:00",
    "machine": "NC"
  }
])json";

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string gunzip(const fs::path& gz)
{
    gzFile f = gzopen(gz.c_str(), "rb");
    if (!f)
        return {};
    std::string out;
    char        buf[4096];
    int         n;
    while ((n = gzread(f, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    gzclose(f);
    return out;
}

/// Concatenated content of every `<stem>.*.log` / `.log.gz` file in @p dir.
std::string familyText(const fs::path& dir, const std::string& stem)
{
    std::string out;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const auto n = e.path().filename().string();
        if (n.rfind(stem + ".", 0) != 0)
            continue;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".gz") == 0)
            out += gunzip(e.path());
        else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
            out += readFile(e.path());
    }
    return out;
}

std::vector<std::string> listNames(const fs::path& dir)
{
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir))
        out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

/// Every non-empty line of @p text carries the logger tag @p tag.
::testing::AssertionResult allLinesTagged(const std::string& text, const std::string& tag)
{
    std::istringstream in(text);
    std::string        line;
    int                n = 0;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        ++n;
        if (line.find(tag) == std::string::npos)
            return ::testing::AssertionFailure() << "line without '" << tag << "': " << line;
    }
    if (n == 0)
        return ::testing::AssertionFailure() << "no lines";
    return ::testing::AssertionSuccess();
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ControllerFileLoggingIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        logDir_ = fs::temp_directory_path() /
                  ("mldp_ctrl_log_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &grpcPort_);
        builder.RegisterService(&svc_);
        grpcServer_ = builder.BuildAndStart();
        ASSERT_TRUE(grpcServer_);
        calendar_.setResponse("lcls", kEvents);
        calendar_.start();

        // Quiet, debug-level default logger so shared helpers (HTTP client) log too.
        savedDefault_ = spdlog::default_logger();
        auto def      = std::make_shared<spdlog::logger>("mldp_pvxs_driver");
        def->set_level(spdlog::level::debug);
        spdlog::set_default_logger(def);
    }

    void TearDown() override
    {
        if (controller_)
            controller_->stop();
        controller_.reset();
        mldp_pvxs_driver::util::log::setLoggerFactory({});
        mldp_pvxs_driver::util::log::setLogger(nullptr);
        spdlog::drop_all();
        if (compressor_)
            compressor_->stop();
        spdlog::set_default_logger(savedDefault_);
        calendar_.stop();
        if (grpcServer_)
            grpcServer_->Shutdown();
        std::error_code ec;
        fs::remove_all(logDir_, ec);
    }

    std::string yaml(const std::string& loggingBlock) const
    {
        std::ostringstream ss;
        ss << "writer:\n"
           << "  mldp-configuration:\n"
           << "    - name: cal-writer-log\n"
           << "      thread-pool: 1\n"
           << "      deadline-seconds: 5\n"
           << "      mldp-annotation-pool:\n"
           << "        annotation-url: 127.0.0.1:" << grpcPort_ << "\n"
           << "        min-conn: 1\n"
           << "        max-conn: 1\n"
           << "reader:\n"
           << "  - slac-calendar:\n"
           << "      - name: cal-reader-log\n"
           << "        base-url: " << calendar_.baseUrl() << "\n"
           << "        accel:\n"
           << "          - lcls\n"
           << "        lookahead-days: 30\n"
           << "        lookback-days: 1\n"
           << "        fetch-window-days: 32\n"
           << "        rescan-interval-sec: 0.0\n"
           << "        connect-timeout-sec: 5\n"
           << "        total-timeout-sec: 15\n"
           << "        tls-verify-peer: false\n"
           << "        tls-verify-host: false\n"
           << loggingBlock;
        return ss.str();
    }

    /// Full pipeline: parse YAML → install logging → run controller until saves land → stop.
    void runPipeline(const std::string& loggingBlock,
                     const std::function<void(LoggingConfig&)>& tweak = {})
    {
        const auto config = makeConfigFromYaml(yaml(loggingBlock));
        auto       cfg    = LoggingConfig::parse(config);
        ASSERT_TRUE(cfg.fileLoggingEnabled());
        cfg.console = false;
        if (tweak)
            tweak(cfg);
        compressor_ = installFileLogging(cfg, "[%Y-%m-%d %H:%M:%S.%e] [%-32!n] [%-8l] %v");

        controller_ = MLDPPVXSController::create(config);
        controller_->start();
        ASSERT_TRUE(waitFor([this] { return svc_.configs.load() >= 2 && svc_.activations.load() >= 2; },
                            std::chrono::seconds(10)))
            << "configs=" << svc_.configs.load() << " activations=" << svc_.activations.load();

        controller_->stop(); // drains queues; components log their stop lines
        controller_.reset();
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
        compressor_->waitIdle();
    }

    fs::path                            logDir_;
    LoggingTestAnnotationSvc            svc_;
    int                                 grpcPort_{0};
    std::unique_ptr<grpc::Server>       grpcServer_;
    MockCalendarHttpServer              calendar_;
    std::shared_ptr<MLDPPVXSController> controller_;
    std::shared_ptr<spdlog::logger>     savedDefault_;
    std::shared_ptr<LogCompressor>      compressor_;
};

// ---------------------------------------------------------------------------
// Per-component files: each component's lines in its own file, all in main
// ---------------------------------------------------------------------------

TEST_F(ControllerFileLoggingIntegrationTest, PerComponentFilesSplitRealControllerLogs)
{
    runPipeline("logging:\n"
                "  log-file: " + (logDir_ / "driver.log").string() + "\n"
                "  per-component:\n"
                "    readers: true\n"
                "    writers: true\n"
                "    controller: true\n");

    const auto names = listNames(logDir_);
    SCOPED_TRACE(::testing::PrintToString(names));

    const auto reader     = familyText(logDir_, "reader-slac-calendar_cal-reader-log");
    const auto writer     = familyText(logDir_, "writer-configuration_cal-writer-log");
    const auto controller = familyText(logDir_, "controller-default");
    const auto main       = familyText(logDir_, "driver");

    // Reader file: only the reader, incl. the skipped-event warning with its details.
    EXPECT_TRUE(allLinesTagged(reader, "[reader:slac-calendar:cal-reader-"));
    EXPECT_NE(reader.find("skipping event with empty program_name"), std::string::npos) << reader;
    EXPECT_NE(reader.find("end='2026-05-30T09:00:00-07:00'"), std::string::npos) << reader;
    EXPECT_NE(reader.find("calendar='NC-XPP'"), std::string::npos) << reader;

    // Writer file: only the writer's lifecycle lines.
    EXPECT_TRUE(allLinesTagged(writer, "[configuration_writer:cal-writer"));
    EXPECT_NE(writer.find("MLDPConfigurationWriter 'cal-writer-log' pool ready"), std::string::npos);
    EXPECT_NE(writer.find("MLDPConfigurationWriter 'cal-writer-log' stopped"), std::string::npos);

    // Controller file: only the controller.
    EXPECT_TRUE(allLinesTagged(controller, "[controller.default"));
    EXPECT_NE(controller.find("Controller is starting"), std::string::npos);
    EXPECT_NE(controller.find("Controller stopped"), std::string::npos);

    // Main file: everything, including shared helpers that have no own file.
    for (const char* needle : {"skipping event with empty program_name", "pool ready",
                               "Controller is starting", "[util:http:curl", "HTTP GET"})
        EXPECT_NE(main.find(needle), std::string::npos) << needle;
    EXPECT_EQ(reader.find("[util:http:curl"), std::string::npos);

    // Exactly 4 active log families, all in the log-file folder.
    int active = 0;
    for (const auto& n : names)
        active += (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0) ? 1 : 0;
    EXPECT_EQ(active, 4);
}

// ---------------------------------------------------------------------------
// Default: only the main log file
// ---------------------------------------------------------------------------

TEST_F(ControllerFileLoggingIntegrationTest, DefaultWritesOnlyMainFile)
{
    runPipeline("logging:\n  log-file: " + (logDir_ / "run.log").string() + "\n");

    const auto names = listNames(logDir_);
    ASSERT_EQ(names.size(), 1u) << ::testing::PrintToString(names);
    EXPECT_EQ(names[0].rfind("run.", 0), 0u);

    const auto main = readFile(logDir_ / names[0]);
    for (const char* needle : {"[reader:slac-calendar:cal-reader-", "[configuration_writer:cal-writer",
                               "[controller.default", "skipping event with empty program_name"})
        EXPECT_NE(main.find(needle), std::string::npos) << needle;
}

// ---------------------------------------------------------------------------
// Rotation + gzip during a real run; no line lost across rotated files
// ---------------------------------------------------------------------------

TEST_F(ControllerFileLoggingIntegrationTest, RotationCompressesWithoutLosingLines)
{
    runPipeline("logging:\n"
                "  log-file: " + (logDir_ / "driver.log").string() + "\n"
                "  per-run: false\n"
                "  per-component:\n"
                "    controller: true\n"
                "  rotation:\n"
                "    compress: true\n"
                "    max-files: 0\n",
                [](LoggingConfig& c) { c.rotation.maxBytes = 512; }); // MB granularity in YAML

    const auto names = listNames(logDir_);
    SCOPED_TRACE(::testing::PrintToString(names));
    int gz = 0;
    for (const auto& n : names)
        gz += (n.size() > 7 && n.compare(n.size() - 7, 7, ".log.gz") == 0) ? 1 : 0;
    EXPECT_GT(gz, 1);
    EXPECT_TRUE(fs::exists(logDir_ / "driver.log"));
    EXPECT_TRUE(fs::exists(logDir_ / "controller-default.log"));

    // Lines are never split: every line in every file (plain or gz) is complete.
    for (const auto& stem : {std::string("driver"), std::string("controller-default")})
    {
        const auto text = familyText(logDir_, stem);
        std::istringstream in(text);
        std::string        line;
        while (std::getline(in, line))
            if (!line.empty())
                EXPECT_EQ(line.rfind("[20", 0), 0u) << stem << ": " << line;
        EXPECT_NE(text.find("Controller is starting"), std::string::npos) << stem;
        EXPECT_NE(text.find("Controller stopped"), std::string::npos) << stem;
    }
}
