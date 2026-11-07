//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// Retry/resume behaviour of the archiver reader and its per-PV status log.

#include <gtest/gtest.h>

#include "../../../config/test_config_helpers.h"
#include "../../../mock/MockArchiverPbHttpServer.h"
#include "../../../mock/MockDataBus.h"

#include <reader/impl/epics_archiver/ArchiverFetchStatusLog.h>
#include <reader/impl/epics_archiver/EpicsArchiverReader.h>
#include <util/bus/IDataBus.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_archiver::ArchiverFetchStatusLog;
using mldp_pvxs_driver::reader::impl::epics_archiver::EpicsArchiverReader;
using mldp_pvxs_driver::reader::impl::epics_archiver::FetchAttemptRecord;
using mldp_pvxs_driver::reader::impl::epics_archiver::FetchAttemptResult;
using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;
using mldp_pvxs_driver::test::mock::MockDataBus;
using mldp_pvxs_driver::util::bus::asTimeSeries;
using mldp_pvxs_driver::util::bus::getRootSourceName;

using SampleTime = std::pair<uint64_t, uint32_t>;

std::string tempLogPath(const std::string& tag)
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("archiver_status_" + tag + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    std::filesystem::remove(path);
    return path.string();
}

std::vector<nlohmann::json> readJsonLines(const std::string& path)
{
    std::vector<nlohmann::json> out;
    std::ifstream               in(path);
    std::string                 line;
    while (std::getline(in, line))
    {
        if (!line.empty())
        {
            out.push_back(nlohmann::json::parse(line)); // throws on invalid JSON
        }
    }
    return out;
}

// Polls the status log until a summary line is written (historical run finished).
std::vector<nlohmann::json> waitForSummary(const std::string& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (std::filesystem::exists(path))
        {
            auto lines = readJsonLines(path);
            if (!lines.empty() && lines.back().value("type", "") == "summary")
            {
                return lines;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {};
}

std::vector<SampleTime> publishedTimes(const MockDataBus& bus, const std::string& pv)
{
    std::vector<SampleTime> out;
    for (const auto& batch : bus.snapshot())
    {
        if (getRootSourceName(batch) != pv)
        {
            continue;
        }
        for (const auto& frame : asTimeSeries(batch).frames)
        {
            for (const auto& ts : frame.timestamps)
            {
                out.emplace_back(ts.epoch_seconds, ts.nanoseconds);
            }
        }
    }
    return out;
}

std::string readerYaml(const MockArchiverPbHttpServer& server, const std::string& log_path, const std::string& extra,
                       const std::vector<std::string>& pvs)
{
    std::string yaml = std::string(R"(
        name: archiver-retry-test
        hostname: ")") + server.baseUrl() + R"("
        start-date: "2026-02-25T08:00:00.000Z"
        end-date: "2026-02-25T08:00:10.000Z"
        fetch-threads: 1
        fetch-retry-backoff-ms: 10
        status-log-path: ")" + log_path + "\"\n" + extra + "\n        pvs:\n";
    for (const auto& pv : pvs)
    {
        yaml += "          - name: \"" + pv + "\"\n";
    }
    return yaml;
}

// A transient HTTP 500 is retried and the PV is fully ingested; the status log
// records the retrying attempt followed by ok.
TEST(EpicsArchiverReaderRetryTest, TransientHttpErrorIsRetried)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.fail_first_requests = 2;
    gen_cfg.fail_status = 500;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("transient");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0]["result"], "retrying");
    EXPECT_EQ(lines[0]["http_status"], 500);
    EXPECT_EQ(lines[1]["result"], "retrying");
    EXPECT_EQ(lines[2]["result"], "ok");
    EXPECT_EQ(lines[2]["attempt"], 3);
    EXPECT_EQ(lines[2]["max_attempts"], 4);
    EXPECT_GT(lines[2]["samples"].get<std::size_t>(), 0u);
    EXPECT_EQ(lines[3]["ok"], 1);
    EXPECT_EQ(lines[3]["failed"], 0);

    // No samples before success, and all retries reuse the configured start.
    for (const auto& req : server.requestHistory())
    {
        EXPECT_EQ(req.from, "2026-02-25T08:00:00.000Z");
    }
    EXPECT_EQ(publishedTimes(*bus, "TEST:PV:DOUBLE").size(), lines[2]["samples"].get<std::size_t>());
    std::filesystem::remove(log_path);
}

// A connection drop mid-stream restarts the PV from the configured start.
// Only complete batches of the failed attempt are published, and each one is
// identical to the batch the retry sends with the same first sample, so it
// maps to the same MLDP bucket id (pvName-firstSeconds-firstNanos).
TEST(EpicsArchiverReaderRetryTest, MidStreamDropRestartsFromStartWithSameBatches)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 8;
    gen_cfg.max_events_per_second = 8;
    gen_cfg.truncate_first_requests = 1;
    gen_cfg.truncate_after_bytes = 600;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("drop");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "        pv-samples-per-batch: 10", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 3u);
    ASSERT_EQ(lines[0]["result"], "retrying");
    const auto first_attempt_samples = lines[0]["samples"].get<std::size_t>();
    ASSERT_GT(first_attempt_samples, 10u); // at least one complete batch before the drop
    EXPECT_EQ(lines[1]["result"], "ok");
    EXPECT_EQ(lines[1]["resume_source"], "config");
    EXPECT_EQ(lines[1]["from"], "2026-02-25T08:00:00.000Z");

    const auto requests = server.requestHistory();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].from, requests[1].from);

    // Group published batches by bucket key (first sample time).
    std::map<SampleTime, std::vector<std::vector<SampleTime>>> by_key;
    for (const auto& batch : bus->snapshot())
    {
        std::vector<SampleTime> times;
        for (const auto& frame : asTimeSeries(batch).frames)
        {
            for (const auto& ts : frame.timestamps)
            {
                times.emplace_back(ts.epoch_seconds, ts.nanoseconds);
            }
        }
        ASSERT_FALSE(times.empty());
        by_key[times.front()].push_back(std::move(times));
    }

    std::size_t unique_samples = 0;
    std::size_t duplicated_keys = 0;
    for (const auto& [key, batches] : by_key)
    {
        // A key sent twice must carry identical content: never a short batch.
        for (const auto& b : batches)
        {
            EXPECT_EQ(b, batches.front());
        }
        duplicated_keys += batches.size() > 1 ? 1 : 0;
        unique_samples += batches.front().size();
    }
    EXPECT_EQ(duplicated_keys, first_attempt_samples / 10);
    EXPECT_EQ(unique_samples, lines[1]["samples"].get<std::size_t>());
    std::filesystem::remove(log_path);
}

// HTTP 404 is not retried and is reported as skipped_not_found; other PVs continue.
TEST(EpicsArchiverReaderRetryTest, NotFoundIsNotRetried)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.not_found_pvs = {"TEST:PV:MISSING"};
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("notfound");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "", {"TEST:PV:MISSING", "TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0]["pv"], "TEST:PV:MISSING");
    EXPECT_EQ(lines[0]["result"], "skipped_not_found");
    EXPECT_EQ(lines[0]["http_status"], 404);
    EXPECT_EQ(lines[1]["pv"], "TEST:PV:DOUBLE");
    EXPECT_EQ(lines[1]["result"], "ok");
    EXPECT_EQ(lines[2]["not_found_pvs"], nlohmann::json::array({"TEST:PV:MISSING"}));
    EXPECT_EQ(server.requestHistory().size(), 2u);
    std::filesystem::remove(log_path);
}

// Retries exhausted after a partial stream: result is partial, only complete
// batches are published, and the summary lists the PV.
TEST(EpicsArchiverReaderRetryTest, ExhaustedRetriesReportPartial)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 8;
    gen_cfg.max_events_per_second = 8;
    gen_cfg.truncate_first_requests = 1;
    gen_cfg.truncate_after_bytes = 600;
    gen_cfg.fail_first_requests = 0;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("partial");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr,
        makeConfigFromYaml(readerYaml(server, log_path, "        fetch-max-retries: 0\n        pv-samples-per-batch: 10", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0]["result"], "partial");
    EXPECT_TRUE(lines[0]["last_sample_ts"].is_string());
    EXPECT_TRUE(lines[0]["error"].is_string());
    EXPECT_EQ(lines[1]["partial"], 1);
    EXPECT_EQ(lines[1]["partial_pvs"], nlohmann::json::array({"TEST:PV:DOUBLE"}));
    ASSERT_EQ(lines[1]["partial_windows"].size(), 1u);
    EXPECT_EQ(lines[1]["partial_windows"][0]["from"], "2026-02-25T08:00:00.000Z");
    EXPECT_EQ(lines[1]["partial_windows"][0]["to"], "2026-02-25T08:00:10.000Z");
    // Only complete batches are published; the incomplete tail is dropped.
    const auto published = publishedTimes(*bus, "TEST:PV:DOUBLE").size();
    EXPECT_GT(published, 0u);
    EXPECT_EQ(published % 10, 0u);
    EXPECT_LT(published, lines[0]["samples"].get<std::size_t>());
    std::filesystem::remove(log_path);
}

// Retries exhausted with nothing received: result is failed.
TEST(EpicsArchiverReaderRetryTest, ExhaustedRetriesReportFailed)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.fail_first_requests = 100;
    gen_cfg.fail_status = 503;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("failed");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "        fetch-max-retries: 2", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[2]["result"], "failed");
    EXPECT_EQ(lines[2]["attempt"], 3);
    EXPECT_EQ(lines[3]["failed_pvs"], nlohmann::json::array({"TEST:PV:DOUBLE"}));
    EXPECT_EQ(server.requestHistory().size(), 3u);
    EXPECT_TRUE(bus->snapshot().empty());
    std::filesystem::remove(log_path);
}

// The range is split into epoch-aligned windows; a failure re-fetches only
// the failed window, never the windows already done.
TEST(EpicsArchiverReaderRetryTest, WindowedFetchRetriesOnlyFailedWindow)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.fail_first_requests = 1;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("windows");
    auto       bus = std::make_shared<MockDataBus>();
    // Start is not aligned: first window ends at the next 4 s boundary (08:00:04).
    std::string yaml = readerYaml(server, log_path, "        fetch-window-sec: 4", {"TEST:PV:DOUBLE"});
    yaml.replace(yaml.find("08:00:00.000Z"), 13, "08:00:01.000Z");
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, makeConfigFromYaml(yaml));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 5u);

    const auto requests = server.requestHistory();
    ASSERT_EQ(requests.size(), 4u);
    // Window 1 fails once and is retried with the same bounds.
    EXPECT_EQ(requests[0].from, "2026-02-25T08:00:01.000Z");
    EXPECT_EQ(requests[0].to, "2026-02-25T08:00:04.000Z");
    EXPECT_EQ(requests[1].from, requests[0].from);
    EXPECT_EQ(requests[1].to, requests[0].to);
    // Remaining windows are fetched once each.
    EXPECT_EQ(requests[2].from, "2026-02-25T08:00:04.000Z");
    EXPECT_EQ(requests[2].to, "2026-02-25T08:00:08.000Z");
    EXPECT_EQ(requests[3].from, "2026-02-25T08:00:08.000Z");
    EXPECT_EQ(requests[3].to, "2026-02-25T08:00:10.000Z");

    EXPECT_EQ(lines[0]["result"], "retrying");
    EXPECT_EQ(lines[1]["result"], "ok");
    EXPECT_EQ(lines[2]["result"], "ok");
    EXPECT_EQ(lines[3]["result"], "ok");
    EXPECT_EQ(lines[4]["ok"], 3);

    // No batch spans a window boundary.
    for (const auto& batch : bus->snapshot())
    {
        std::vector<SampleTime> times;
        for (const auto& frame : asTimeSeries(batch).frames)
        {
            for (const auto& ts : frame.timestamps)
            {
                times.emplace_back(ts.epoch_seconds, ts.nanoseconds);
            }
        }
        ASSERT_FALSE(times.empty());
        const auto window_of = [](const SampleTime& t) { return t.first / 4; };
        EXPECT_EQ(window_of(times.front()), window_of(times.back()));
    }
    std::filesystem::remove(log_path);
}

// Builds reader YAML with a custom start and optional end (empty = omitted).
std::string rangedYaml(const MockArchiverPbHttpServer& server, const std::string& log_path, const std::string& extra,
                       const std::vector<std::string>& pvs, const std::string& start, const std::string& end)
{
    std::string yaml = readerYaml(server, log_path, extra, pvs);
    yaml.replace(yaml.find("2026-02-25T08:00:00.000Z"), 24, start);
    const auto end_pos = yaml.find("        end-date: \"2026-02-25T08:00:10.000Z\"\n");
    const auto end_len = std::string("        end-date: \"2026-02-25T08:00:10.000Z\"\n").size();
    if (end.empty())
    {
        yaml.erase(end_pos, end_len);
    }
    else
    {
        yaml.replace(end_pos, end_len, "        end-date: \"" + end + "\"\n");
    }
    return yaml;
}

// Aligned start: windows tile the range exactly, no empty leading window.
TEST(EpicsArchiverReaderRetryTest, WindowedFetchAlignedStartSplitsExactly)
{
    MockArchiverPbHttpServer server;
    server.start();

    const auto log_path = tempLogPath("aligned");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "        fetch-window-sec: 5", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 3u);
    const auto requests = server.requestHistory();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].from, "2026-02-25T08:00:00.000Z");
    EXPECT_EQ(requests[0].to, "2026-02-25T08:00:05.000Z");
    EXPECT_EQ(requests[1].from, "2026-02-25T08:00:05.000Z");
    EXPECT_EQ(requests[1].to, "2026-02-25T08:00:10.000Z");
    // Each status line carries its own window bounds.
    EXPECT_EQ(lines[0]["from"], "2026-02-25T08:00:00.000Z");
    EXPECT_EQ(lines[0]["to"], "2026-02-25T08:00:05.000Z");
    EXPECT_EQ(lines[1]["from"], "2026-02-25T08:00:05.000Z");
    EXPECT_EQ(lines[1]["to"], "2026-02-25T08:00:10.000Z");
    EXPECT_EQ(lines[2]["ok"], 2);
    EXPECT_EQ(publishedTimes(*bus, "TEST:PV:DOUBLE").size(),
              lines[0]["samples"].get<std::size_t>() + lines[1]["samples"].get<std::size_t>());
    std::filesystem::remove(log_path);
}

// Range shorter than one window, not crossing a boundary: one request.
TEST(EpicsArchiverReaderRetryTest, WindowedFetchRangeInsideOneWindow)
{
    MockArchiverPbHttpServer server;
    server.start();

    const auto log_path = tempLogPath("inside");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr,
        makeConfigFromYaml(rangedYaml(server, log_path, "        fetch-window-sec: 3600", {"TEST:PV:DOUBLE"},
                                      "2026-02-25T08:00:01.000Z", "2026-02-25T08:00:09.000Z")));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 2u);
    const auto requests = server.requestHistory();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].from, "2026-02-25T08:00:01.000Z");
    EXPECT_EQ(requests[0].to, "2026-02-25T08:00:09.000Z");
    EXPECT_EQ(lines[1]["ok"], 1);
    std::filesystem::remove(log_path);
}

// fetch-window-sec: 0 disables windowing: whole range in one request.
TEST(EpicsArchiverReaderRetryTest, WindowZeroFetchesWholeRangeInOneRequest)
{
    MockArchiverPbHttpServer server;
    server.start();

    const auto log_path = tempLogPath("zero");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "        fetch-window-sec: 0", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 2u);
    const auto requests = server.requestHistory();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].from, "2026-02-25T08:00:00.000Z");
    EXPECT_EQ(requests[0].to, "2026-02-25T08:00:10.000Z");
    EXPECT_EQ(lines[0]["to"], "2026-02-25T08:00:10.000Z");
    EXPECT_EQ(lines[1]["ok"], 1);
    std::filesystem::remove(log_path);
}

// No end-date: windowing is skipped, one open-ended request.
TEST(EpicsArchiverReaderRetryTest, NoEndDateSkipsWindowing)
{
    MockArchiverPbHttpServer server;
    server.start();

    const auto log_path = tempLogPath("noend");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr,
        makeConfigFromYaml(
            rangedYaml(server, log_path, "        fetch-window-sec: 1", {"TEST:PV:DOUBLE"}, "2026-02-25T08:00:00.000Z", "")));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 2u);
    const auto requests = server.requestHistory();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].from, "2026-02-25T08:00:00.000Z");
    EXPECT_FALSE(requests[0].to.has_value());
    EXPECT_TRUE(lines[0]["to"].is_null());
    std::filesystem::remove(log_path);
}

// Partial in first window only: summary lists that window; later windows ok.
TEST(EpicsArchiverReaderRetryTest, WindowedPartialReportsOnlyAffectedWindow)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 8;
    gen_cfg.max_events_per_second = 8;
    gen_cfg.truncate_first_requests = 1;
    gen_cfg.truncate_after_bytes = 600;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("wpartial");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr,
        makeConfigFromYaml(readerYaml(server, log_path, "        fetch-window-sec: 5\n        fetch-max-retries: 0",
                                      {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0]["result"], "partial");
    EXPECT_EQ(lines[1]["result"], "ok");
    const auto& summary = lines[2];
    EXPECT_EQ(summary["ok"], 1);
    EXPECT_EQ(summary["partial"], 1);
    EXPECT_EQ(summary["partial_pvs"], nlohmann::json::array({"TEST:PV:DOUBLE"}));
    ASSERT_EQ(summary["partial_windows"].size(), 1u);
    EXPECT_EQ(summary["partial_windows"][0]["pv"], "TEST:PV:DOUBLE");
    EXPECT_EQ(summary["partial_windows"][0]["from"], "2026-02-25T08:00:00.000Z");
    EXPECT_EQ(summary["partial_windows"][0]["to"], "2026-02-25T08:00:05.000Z");
    std::filesystem::remove(log_path);
}

// Every window fails: each listed in failed_windows, PV listed once.
TEST(EpicsArchiverReaderRetryTest, WindowedFailuresListEachWindowOncePerPv)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.fail_first_requests = 100;
    gen_cfg.fail_status = 503;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("wfailed");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr,
        makeConfigFromYaml(readerYaml(server, log_path, "        fetch-window-sec: 4\n        fetch-max-retries: 1",
                                      {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    // 3 windows x (retrying + failed) + summary.
    ASSERT_EQ(lines.size(), 7u);
    EXPECT_EQ(server.requestHistory().size(), 6u);
    const auto& summary = lines.back();
    EXPECT_EQ(summary["failed"], 3);
    EXPECT_EQ(summary["ok"], 0);
    EXPECT_EQ(summary["failed_pvs"], nlohmann::json::array({"TEST:PV:DOUBLE"}));
    ASSERT_EQ(summary["failed_windows"].size(), 3u);
    EXPECT_EQ(summary["failed_windows"][0]["from"], "2026-02-25T08:00:00.000Z");
    EXPECT_EQ(summary["failed_windows"][0]["to"], "2026-02-25T08:00:04.000Z");
    EXPECT_EQ(summary["failed_windows"][1]["from"], "2026-02-25T08:00:04.000Z");
    EXPECT_EQ(summary["failed_windows"][1]["to"], "2026-02-25T08:00:08.000Z");
    EXPECT_EQ(summary["failed_windows"][2]["from"], "2026-02-25T08:00:08.000Z");
    EXPECT_EQ(summary["failed_windows"][2]["to"], "2026-02-25T08:00:10.000Z");
    EXPECT_TRUE(bus->snapshot().empty());
    std::filesystem::remove(log_path);
}

// 404 in windowed mode: reported as not_found, other PVs ok.
TEST(EpicsArchiverReaderRetryTest, WindowedNotFoundReportsWindows)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.not_found_pvs = {"TEST:PV:MISSING"};
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("wnotfound");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr,
        makeConfigFromYaml(
            readerYaml(server, log_path, "        fetch-window-sec: 5", {"TEST:PV:MISSING", "TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_FALSE(lines.empty());
    const auto& summary = lines.back();
    EXPECT_EQ(summary["not_found_pvs"], nlohmann::json::array({"TEST:PV:MISSING"}));
    ASSERT_FALSE(summary["not_found_windows"].empty());
    for (const auto& w : summary["not_found_windows"])
    {
        EXPECT_EQ(w["pv"], "TEST:PV:MISSING");
        EXPECT_TRUE(w["to"].is_string());
    }
    EXPECT_EQ(summary["ok"], 2);
    for (std::size_t i = 0; i + 1 < lines.size(); ++i)
    {
        if (lines[i]["pv"] == "TEST:PV:MISSING")
        {
            EXPECT_EQ(lines[i]["result"], "skipped_not_found");
            EXPECT_EQ(lines[i]["http_status"], 404);
        }
    }
    EXPECT_FALSE(publishedTimes(*bus, "TEST:PV:DOUBLE").empty());
    EXPECT_TRUE(publishedTimes(*bus, "TEST:PV:MISSING").empty());
    std::filesystem::remove(log_path);
}

// The archiver prepends the last sample before the requested start. In
// windowed historical fetches that sample belongs to the previous window, so
// it must be dropped: no duplicates, and no batch reaches before its window.
TEST(EpicsArchiverReaderRetryTest, WindowedFetchDropsSampleBeforeWindowStart)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.emit_prior_sample = true;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();

    const auto log_path = tempLogPath("prior");
    auto       bus = std::make_shared<MockDataBus>();
    auto       reader = std::make_unique<EpicsArchiverReader>(
        bus, nullptr, makeConfigFromYaml(readerYaml(server, log_path, "        fetch-window-sec: 5", {"TEST:PV:DOUBLE"})));

    const auto lines = waitForSummary(log_path, std::chrono::seconds(5));
    ASSERT_EQ(lines.size(), 3u);
    ASSERT_EQ(server.requestHistory().size(), 2u);

    // first_sample_ts is never before the window start.
    for (std::size_t i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(lines[i]["first_sample_ts"].is_string());
        EXPECT_GE(lines[i]["first_sample_ts"].get<std::string>(), lines[i]["from"].get<std::string>());
    }

    // 1772006400 = 2026-02-25T08:00:00Z (range start).
    const uint64_t kStart = 1772006400;
    auto           times = publishedTimes(*bus, "TEST:PV:DOUBLE");
    ASSERT_FALSE(times.empty());
    EXPECT_EQ(times.size(), lines[0]["samples"].get<std::size_t>() + lines[1]["samples"].get<std::size_t>());
    std::sort(times.begin(), times.end());
    EXPECT_GE(times.front().first, kStart);
    EXPECT_EQ(std::adjacent_find(times.begin(), times.end()), times.end()) << "duplicate sample published";

    // Every batch stays inside one 5 s window.
    for (const auto& batch : bus->snapshot())
    {
        std::vector<SampleTime> bt;
        for (const auto& frame : asTimeSeries(batch).frames)
        {
            for (const auto& ts : frame.timestamps)
            {
                bt.emplace_back(ts.epoch_seconds, ts.nanoseconds);
            }
        }
        ASSERT_FALSE(bt.empty());
        EXPECT_EQ(bt.front().first / 5, bt.back().first / 5);
    }
    std::filesystem::remove(log_path);
}

// Summary: PV names deduped across windows, windows kept in record order,
// retrying/ok lines not listed as problem windows.
TEST(ArchiverFetchStatusLogTest, SummaryDedupesPvsAndListsAllWindows)
{
    const auto path = tempLogPath("unit_windows");
    {
        ArchiverFetchStatusLog log(path);
        FetchAttemptRecord     rec;
        rec.reader = "r";
        rec.resume_source = "config";
        const auto add = [&](const std::string& pv, const std::string& from, const std::string& to, FetchAttemptResult r)
        {
            rec.pv = pv;
            rec.from = from;
            rec.to = to;
            rec.result = r;
            log.record(rec);
        };
        add("A", "2026-01-01T00:00:00.000Z", "2026-01-02T00:00:00.000Z", FetchAttemptResult::Failed);
        add("A", "2026-01-02T00:00:00.000Z", "2026-01-03T00:00:00.000Z", FetchAttemptResult::Retrying);
        add("A", "2026-01-02T00:00:00.000Z", "2026-01-03T00:00:00.000Z", FetchAttemptResult::Ok);
        add("A", "2026-01-03T00:00:00.000Z", "2026-01-04T00:00:00.000Z", FetchAttemptResult::Failed);
        add("B", "2026-01-01T00:00:00.000Z", "2026-01-02T00:00:00.000Z", FetchAttemptResult::Partial);
        add("B", "2026-01-02T00:00:00.000Z", "2026-01-03T00:00:00.000Z", FetchAttemptResult::Partial);
        add("C", "2026-01-01T00:00:00.000Z", "2026-01-02T00:00:00.000Z", FetchAttemptResult::SkippedNotFound);
        log.writeSummary("r");
    }
    const auto lines = readJsonLines(path);
    ASSERT_EQ(lines.size(), 8u);
    for (std::size_t i = 0; i < 7; ++i)
    {
        EXPECT_TRUE(lines[i]["from"].is_string());
        EXPECT_TRUE(lines[i]["to"].is_string());
    }
    const auto& s = lines[7];
    EXPECT_EQ(s["type"], "summary");
    EXPECT_EQ(s["ok"], 1);
    EXPECT_EQ(s["failed"], 2);
    EXPECT_EQ(s["partial"], 2);
    EXPECT_EQ(s["not_found"], 1);
    EXPECT_EQ(s["failed_pvs"], nlohmann::json::array({"A"}));
    EXPECT_EQ(s["partial_pvs"], nlohmann::json::array({"B"}));
    EXPECT_EQ(s["not_found_pvs"], nlohmann::json::array({"C"}));
    ASSERT_EQ(s["failed_windows"].size(), 2u);
    EXPECT_EQ(s["failed_windows"][0]["from"], "2026-01-01T00:00:00.000Z");
    EXPECT_EQ(s["failed_windows"][1]["from"], "2026-01-03T00:00:00.000Z");
    EXPECT_EQ(s["failed_windows"][1]["to"], "2026-01-04T00:00:00.000Z");
    ASSERT_EQ(s["partial_windows"].size(), 2u);
    EXPECT_EQ(s["partial_windows"][1]["to"], "2026-01-03T00:00:00.000Z");
    ASSERT_EQ(s["not_found_windows"].size(), 1u);
    EXPECT_EQ(s["not_found_windows"][0]["pv"], "C");
    std::filesystem::remove(path);
}

// Status log lines are valid JSON and the summary counts final results only.
TEST(ArchiverFetchStatusLogTest, WritesJsonLinesAndSummary)
{
    const auto path = tempLogPath("unit");
    {
        ArchiverFetchStatusLog log(path);
        FetchAttemptRecord     rec;
        rec.reader = "r";
        rec.pv = "A";
        rec.from = "2026-01-01T00:00:00.000Z";
        rec.resume_source = "config";
        rec.result = FetchAttemptResult::Retrying;
        rec.error = std::string("bad \xff utf8");
        log.record(rec);
        rec.result = FetchAttemptResult::Ok;
        rec.error.reset();
        log.record(rec);
        rec.pv = "B";
        rec.result = FetchAttemptResult::Failed;
        log.record(rec);
        log.writeSummary("r");
    }
    const auto lines = readJsonLines(path);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0]["result"], "retrying");
    EXPECT_TRUE(lines[0]["to"].is_null());
    EXPECT_EQ(lines[3]["type"], "summary");
    EXPECT_EQ(lines[3]["ok"], 1);
    EXPECT_EQ(lines[3]["failed"], 1);
    EXPECT_EQ(lines[3]["failed_pvs"], nlohmann::json::array({"B"}));
    EXPECT_EQ(lines[3]["failed_windows"][0]["pv"], "B");
    EXPECT_TRUE(lines[3]["failed_windows"][0]["to"].is_null());
    std::filesystem::remove(path);
}

} // namespace
