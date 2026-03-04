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
#include <memory>
#include <mutex>
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
        received.emplace_back(std::move(batch));
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
        return received;
    }

private:
    mutable std::mutex      mu_;
    std::vector<EventBatch> received;
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

// Verifies the reader fetches PB/HTTP data and publishes parsed samples to the event bus.
TEST(EpicsArchiverReaderHttpIntegrationTest, FetchesPbHttpStreamAndPublishesBusEvents)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.min_value = -5.0;
    gen_cfg.max_value = 5.0;
    gen_cfg.random_seed = 99;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-http-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));

    EXPECT_EQ(reader->name(), "archiver-http-test");

    const auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].frames.size(), 4u);
    EXPECT_EQ(batches[0].root_source, "TEST:PV:DOUBLE");

    uint64_t prev_epoch = 0;
    uint64_t prev_nano = 0;
    for (size_t i = 0; i < batches[0].frames.size(); ++i)
    {
        const auto& frame = batches[0].frames[i];
        ASSERT_GT(frame.doublecolumns_size(), 0);
        ASSERT_GT(frame.doublecolumns(0).values_size(), 0);
        EXPECT_EQ(frame.doublecolumns(0).name(), "TEST:PV:DOUBLE");
        EXPECT_GE(frame.doublecolumns(0).values(0), gen_cfg.min_value);
        EXPECT_LE(frame.doublecolumns(0).values(0), gen_cfg.max_value);
        ASSERT_TRUE(frame.has_datatimestamps());
        ASSERT_GT(frame.datatimestamps().timestamplist().timestamps_size(), 0);
        const auto epoch = frame.datatimestamps().timestamplist().timestamps(0).epochseconds();
        const auto nano = frame.datatimestamps().timestamplist().timestamps(0).nanoseconds();
        EXPECT_GT(epoch, 0u);
        EXPECT_LT(nano, 1'000'000'000u);
        if (i > 0)
        {
            const bool nondecreasing =
                (epoch > prev_epoch) || (epoch == prev_epoch && nano >= prev_nano);
            EXPECT_TRUE(nondecreasing);
        }
        prev_epoch = epoch;
        prev_nano = nano;
    }

    const auto req = server.lastRequest();
    ASSERT_TRUE(req.pv.has_value());
    ASSERT_TRUE(req.from.has_value());
    EXPECT_EQ(*req.pv, "TEST:PV:DOUBLE");
    EXPECT_EQ(*req.from, "2026-02-25T08:00:00.000Z");
    EXPECT_FALSE(req.to.has_value());
}

// Verifies the reader includes the optional end-date as the archiver 'to' query parameter.
TEST(EpicsArchiverReaderHttpIntegrationTest, IncludesOptionalToQueryWhenConfigured)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-http-test-to
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        end-date: "2026-02-25T08:00:02.000Z"
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));
    EXPECT_EQ(reader->name(), "archiver-http-test-to");

    const auto req = server.lastRequest();
    ASSERT_TRUE(req.to.has_value());
    EXPECT_EQ(*req.to, "2026-02-25T08:00:02.000Z");
}

// Verifies published batches are split by historical sample timestamps and still include the 'to' query.
TEST(EpicsArchiverReaderHttpIntegrationTest, SplitsPublishedBatchesByHistoricalSampleTime)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 5;
    gen_cfg.min_value = -5.0;
    gen_cfg.max_value = 5.0;
    gen_cfg.random_seed = 123;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-http-batch-split
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        end-date: "2026-02-25T08:00:05.000Z"
        batch-duration-sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));
    EXPECT_EQ(reader->name(), "archiver-http-batch-split");

    const auto req = server.lastRequest();
    ASSERT_TRUE(req.to.has_value());
    EXPECT_EQ(*req.to, "2026-02-25T08:00:05.000Z");

    const auto batches = bus->snapshot();
    ASSERT_GT(batches.size(), 1u);

    size_t   total_events = 0u;
    uint64_t prev_epoch = 0u;
    uint64_t prev_nano = 0u;
    bool     first = true;

    for (const auto& batch : batches)
    {
        ASSERT_FALSE(batch.frames.empty());
        const auto& first_frame = batch.frames.front();
        const auto& last_frame = batch.frames.back();
        ASSERT_TRUE(first_frame.has_datatimestamps());
        ASSERT_TRUE(last_frame.has_datatimestamps());
        const auto& first_ts = first_frame.datatimestamps().timestamplist().timestamps(0);
        const auto& last_ts = last_frame.datatimestamps().timestamplist().timestamps(0);

        const uint64_t first_ns = first_ts.epochseconds() * 1'000'000'000ULL + first_ts.nanoseconds();
        const uint64_t last_ns = last_ts.epochseconds() * 1'000'000'000ULL + last_ts.nanoseconds();
        EXPECT_LE(last_ns - first_ns, 1'000'000'000ULL);

        for (const auto& frame : batch.frames)
        {
            ASSERT_TRUE(frame.has_datatimestamps());
            total_events++;
            const auto& ts = frame.datatimestamps().timestamplist().timestamps(0);
            if (!first)
            {
                const bool nondecreasing =
                    (ts.epochseconds() > prev_epoch) || (ts.epochseconds() == prev_epoch && ts.nanoseconds() >= prev_nano);
                EXPECT_TRUE(nondecreasing);
            }
            prev_epoch = ts.epochseconds();
            prev_nano = ts.nanoseconds();
            first = false;
        }
    }

    EXPECT_EQ(total_events, 20u);
}

// Verifies destroying the reader during a long PB/HTTP download cancels the in-flight HTTP stream.
TEST(EpicsArchiverReaderHttpIntegrationTest, DestructorAbortsOngoingLongDownload)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 20;
    gen_cfg.max_events_per_second = 20;
    gen_cfg.open_ended_duration_sec = 30;
    gen_cfg.stream_chunk_delay_ms = 10;
    gen_cfg.random_seed = 456;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-http-cancel-on-destroy
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    ASSERT_TRUE(waitForMockRequestStart(server, std::chrono::seconds(2)));

    const auto destroy_start = std::chrono::steady_clock::now();
    reader.reset(); // destructor should cancel streamGet and join promptly
    const auto destroy_elapsed = std::chrono::steady_clock::now() - destroy_start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(destroy_elapsed), std::chrono::seconds(2));
    ASSERT_TRUE(server.waitForLastResponseComplete(std::chrono::seconds(2)));
    ASSERT_TRUE(server.lastResponseSuccess().has_value());
    EXPECT_FALSE(*server.lastResponseSuccess());
}

} // namespace
