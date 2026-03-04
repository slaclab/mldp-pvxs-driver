//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include "../../../config/test_config_helpers.h"
#include "MockArchiverPbHttpServer.h"

#include <reader/impl/epics_archiver/EpicsArchiverReader.h>
#include <util/bus/IDataBus.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_archiver::EpicsArchiverReader;
using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;
using mldp_pvxs_driver::util::bus::IDataBus;

class MockEventBusPush final : public IDataBus
{
public:
    using EventBatch = IDataBus::EventBatch;

    bool push(EventBatch batch) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        received_.emplace_back(std::move(batch));
        return true;
    }

    std::vector<SourceInfo> querySourcesInfo(const std::set<std::string>&) override
    {
        return {};
    }

    std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>> querySourcesData(
        const std::set<std::string>&,
        const mldp_pvxs_driver::util::bus::QuerySourcesDataOptions&) override
    {
        return std::nullopt;
    }

    std::vector<EventBatch> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return received_;
    }

private:
    mutable std::mutex      mu_;
    std::vector<EventBatch> received_;
};

bool waitForMockRequestStartAndCompletion(const MockArchiverPbHttpServer& server,
                                          std::chrono::milliseconds       timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!server.lastRequest().path.empty())
        {
            return server.waitForLastResponseComplete(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForMockRequestStart(const MockArchiverPbHttpServer& server, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!server.lastRequest().path.empty())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForAtLeastPublishedBatches(const MockEventBusPush& bus, size_t min_batches, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (bus.snapshot().size() >= min_batches)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return bus.snapshot().size() >= min_batches;
}

int64_t parseIso8601UtcMillisToEpochMs(const std::string& s)
{
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ", &year, &month, &day, &hour, &min, &sec, &ms) != 7)
    {
        return -1;
    }

    // Howard Hinnant civil date conversion.
    auto daysFromCivil = [](int y, unsigned m, unsigned d) -> int64_t
    {
        y -= m <= 2;
        const int      era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    };

    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return ((days * 24 + hour) * 60 + min) * 60 * 1000 + static_cast<int64_t>(sec) * 1000 + ms;
}

// Verifies periodic_tail mode polls repeatedly with explicit from/to query parameters.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, PollsRepeatedlyWithExplicitWindow)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 2u, std::chrono::seconds(4)));
    ASSERT_TRUE(server.waitForRequestCount(2u, std::chrono::seconds(1)));

    const auto req = server.lastRequest();
    ASSERT_TRUE(req.pv.has_value());
    ASSERT_TRUE(req.from.has_value());
    ASSERT_TRUE(req.to.has_value());
    EXPECT_EQ(*req.pv, "TEST:PV:DOUBLE");
    EXPECT_FALSE(req.from->empty());
    EXPECT_FALSE(req.to->empty());

    reader.reset();
}

// Verifies periodic_tail fetches immediately and defaults lookback-sec to poll-interval-sec with contiguous windows.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, DefaultsLookbackToPollIntervalAndUsesContiguousWindows)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail-default-lookback
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);

    const auto start = std::chrono::steady_clock::now();
    auto       reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStart(server, std::chrono::milliseconds(500)));
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start),
              std::chrono::milliseconds(500));

    ASSERT_TRUE(server.waitForRequestCount(2u, std::chrono::seconds(4)));
    const auto history = server.requestHistory();
    ASSERT_GE(history.size(), 2u);
    ASSERT_TRUE(history[0].from.has_value());
    ASSERT_TRUE(history[0].to.has_value());
    ASSERT_TRUE(history[1].from.has_value());
    ASSERT_TRUE(history[1].to.has_value());

    // When lookback defaults to poll-interval-sec, the implementation uses the
    // previous iteration end timestamp as the next iteration start.
    EXPECT_EQ(*history[1].from, *history[0].to);

    reader.reset();
}

// Verifies periodic_tail honors an explicit shorter lookback window than poll interval.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, HonorsExplicitLookbackShorterThanPollInterval)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail-short-lookback
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 2
        lookback-sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));
    const auto req = server.lastRequest();
    ASSERT_TRUE(req.from.has_value());
    ASSERT_TRUE(req.to.has_value());

    const int64_t from_ms = parseIso8601UtcMillisToEpochMs(*req.from);
    const int64_t to_ms = parseIso8601UtcMillisToEpochMs(*req.to);
    ASSERT_GE(from_ms, 0);
    ASSERT_GE(to_ms, 0);
    const int64_t duration_ms = to_ms - from_ms;

    // Millisecond truncation should keep this very close to the configured 1s lookback.
    EXPECT_GE(duration_ms, 999);
    EXPECT_LE(duration_ms, 1001);

    reader.reset();
}

// Verifies batch-duration-sec controls batch splitting in periodic_tail mode as well.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, UsesBatchDurationSecForPeriodicTailBatchSplitting)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail-batch-window
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 2
        lookback-sec: 2
        batch-duration-sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));

    const auto batches = bus->snapshot();
    // A 2-second fetch window with 1-second batch_duration should split into
    // multiple published batches based on historical sample timestamps.
    ASSERT_GT(batches.size(), 1u);

    reader.reset();
}

// Verifies periodic_tail batches respect batch-duration-sec, allowing the final
// batch to be partial at iteration boundary.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, PeriodicTailBatchSpansMatchConfiguredWindowExceptFinalPartial)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail-batch-window-span-check
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 10
        lookback-sec: 2
        batch-duration-sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    // Only the immediate first fetch should run within this wait; poll interval
    // is long enough to avoid a second iteration.
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));

    const auto batches = bus->snapshot();
    ASSERT_GT(batches.size(), 1u);

    size_t total_events = 0u;
    for (size_t i = 0; i < batches.size(); ++i)
    {
        ASSERT_FALSE(batches[i].frames.empty());
        total_events += batches[i].frames.size();
        const auto& first_frame = batches[i].frames.front();
        const auto& last_frame = batches[i].frames.back();
        ASSERT_TRUE(first_frame.has_datatimestamps());
        ASSERT_TRUE(last_frame.has_datatimestamps());
        const auto& first_ts = first_frame.datatimestamps().timestamplist().timestamps(0);
        const auto& last_ts = last_frame.datatimestamps().timestamplist().timestamps(0);

        const uint64_t first_ns = first_ts.epochseconds() * 1'000'000'000ULL + first_ts.nanoseconds();
        const uint64_t last_ns = last_ts.epochseconds() * 1'000'000'000ULL + last_ts.nanoseconds();
        const uint64_t span_ns = last_ns - first_ns;

        if (i + 1 < batches.size())
        {
            // Non-final batches must respect the configured 1-second window.
            EXPECT_LE(span_ns, 1'000'000'000ULL);
        }
        else
        {
            // Final batch may be partial due to iteration/chunk boundary flush.
            EXPECT_LE(span_ns, 1'000'000'000ULL);
        }
    }

    // 2-second lookback with 4 events/sec deterministic mock -> 8 samples total.
    EXPECT_EQ(total_events, 8u);

    reader.reset();
}

// Verifies a 10s periodic tail poller covers 30s of data with three contiguous 10s fetch windows.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, TenSecondPollingCoversThirtySecondsWithContiguousWindows)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail-10s
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 10
        batch-duration-sec: 10
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    // Fetch happens immediately, then every 10s. Three requests should arrive within ~20s.
    ASSERT_TRUE(server.waitForRequestCount(3u, std::chrono::seconds(25)));
    const auto history = server.requestHistory();
    ASSERT_GE(history.size(), 3u);

    for (size_t i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(history[i].from.has_value()) << "missing from on request " << i;
        ASSERT_TRUE(history[i].to.has_value()) << "missing to on request " << i;
    }

    const int64_t from0_ms = parseIso8601UtcMillisToEpochMs(*history[0].from);
    const int64_t to0_ms = parseIso8601UtcMillisToEpochMs(*history[0].to);
    const int64_t from1_ms = parseIso8601UtcMillisToEpochMs(*history[1].from);
    const int64_t to1_ms = parseIso8601UtcMillisToEpochMs(*history[1].to);
    const int64_t from2_ms = parseIso8601UtcMillisToEpochMs(*history[2].from);
    const int64_t to2_ms = parseIso8601UtcMillisToEpochMs(*history[2].to);

    ASSERT_GE(from0_ms, 0);
    ASSERT_GE(to0_ms, 0);
    ASSERT_GE(from1_ms, 0);
    ASSERT_GE(to1_ms, 0);
    ASSERT_GE(from2_ms, 0);
    ASSERT_GE(to2_ms, 0);

    const int64_t d0_ms = to0_ms - from0_ms;
    const int64_t d1_ms = to1_ms - from1_ms;
    const int64_t d2_ms = to2_ms - from2_ms;

    // lookback defaults to poll interval (10s), so each explicit request window
    // should be approximately 10s. Allow scheduler/wakeup jitter and small
    // processing overhead because the reader computes `to` from wall-clock `now`.
    EXPECT_GE(d0_ms, 9999);
    EXPECT_LE(d0_ms, 10500);
    EXPECT_GE(d1_ms, 9999);
    EXPECT_LE(d1_ms, 10500);
    EXPECT_GE(d2_ms, 9999);
    EXPECT_LE(d2_ms, 10500);

    // Contiguous windows: next from equals previous to when lookback == poll interval.
    EXPECT_EQ(*history[1].from, *history[0].to);
    EXPECT_EQ(*history[2].from, *history[1].to);

    // The first three windows together should cover approximately 30s.
    const int64_t total_covered_ms = to2_ms - from0_ms;
    EXPECT_GE(total_covered_ms, 29999);
    EXPECT_LE(total_covered_ms, 31500);

    // batch-duration-sec is set to 10s to align with the nominal fetch window,
    // but a few milliseconds of jitter can push a request window slightly above
    // 10s and trigger an extra split (split rule is strict `elapsed > threshold`).
    // Assert we saw at least one published batch per fetch and no pathological explosion.
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 3u, std::chrono::seconds(2)));
    const auto batches = bus->snapshot();
    EXPECT_GE(batches.size(), 3u);
    EXPECT_LE(batches.size(), 6u);

    reader.reset();
}

// Verifies reader destruction during periodic_tail sleep wakes promptly via condition variable.
TEST(EpicsArchiverReaderPeriodicTailIntegrationTest, DestructorStopsPromptlyWhileWaitingForNextIteration)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-periodic-tail-stop
        hostname: ")") + server.baseUrl() +
                             R"("
        mode: "periodic_tail"
        poll-interval-sec: 2
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto destroy_start = std::chrono::steady_clock::now();
    reader.reset();
    const auto destroy_elapsed = std::chrono::steady_clock::now() - destroy_start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(destroy_elapsed), std::chrono::milliseconds(750));
}

} // namespace
