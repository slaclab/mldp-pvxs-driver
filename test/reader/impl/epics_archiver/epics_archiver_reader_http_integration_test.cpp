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
#include "../../../mock/EpicsArchiverTestUtils.h"
#include "../../../mock/MockArchiverPbHttpServer.h"
#include "../../../mock/MockDataBus.h"

#include <reader/impl/epics_archiver/EpicsArchiverReader.h>
#include <util/bus/IDataBus.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_archiver::EpicsArchiverReader;
using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;
using mldp_pvxs_driver::test::mock::waitForAtLeastPublishedBatches;
using mldp_pvxs_driver::test::mock::waitForMockRequestStart;
using mldp_pvxs_driver::test::mock::waitForMockRequestStartAndCompletion;
using mldp_pvxs_driver::util::bus::IDataBus;
// Backward compatibility alias
using MockEventBusPush = mldp_pvxs_driver::test::mock::MockDataBus;

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

// Verifies a single reader can fetch differently typed PVs and preserve per-PV column families.
TEST(EpicsArchiverReaderHttpIntegrationTest, FetchesMixedTypedPvSetUsingPvSuffixes)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.min_value = -5.0;
    gen_cfg.max_value = 5.0;
    gen_cfg.random_seed = 777;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string pv_string = "TEST:PV:STRING_SCALAR_STRING";
    const std::string pv_int = "TEST:PV:INT_SCALAR_INT";
    const std::string pv_waveform = "TEST:PV:WF_WAVEFORM_DOUBLE";
    const std::string pv_bytes = "TEST:PV:BYTES_V4_GENERIC_BYTES";

    const std::string yaml = std::string(R"(
        name: archiver-http-mixed-types
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pvs:
          - name: ")" + pv_string +
                             R"("
          - name: ")" + pv_int +
                             R"("
          - name: ")" + pv_waveform +
                             R"("
          - name: ")" + pv_bytes +
                             R"("
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    ASSERT_TRUE(server.waitForRequestCount(4u, std::chrono::seconds(2)));
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 4u, std::chrono::seconds(2)));
    EXPECT_EQ(reader->name(), "archiver-http-mixed-types");

    const auto            history = server.requestHistory();
    std::set<std::string> requested_pvs;
    for (const auto& req : history)
    {
        if (req.pv.has_value())
        {
            requested_pvs.insert(*req.pv);
        }
    }
    EXPECT_EQ(requested_pvs, (std::set<std::string>{pv_string, pv_int, pv_waveform, pv_bytes}));

    const auto                                         batches = bus->snapshot();
    std::map<std::string, const IDataBus::EventBatch*> batches_by_source;
    for (const auto& batch : batches)
    {
        batches_by_source.emplace(batch.root_source, &batch);
    }

    ASSERT_TRUE(batches_by_source.count(pv_string));
    ASSERT_TRUE(batches_by_source.count(pv_int));
    ASSERT_TRUE(batches_by_source.count(pv_waveform));
    ASSERT_TRUE(batches_by_source.count(pv_bytes));

    const auto* string_batch = batches_by_source.at(pv_string);
    ASSERT_FALSE(string_batch->frames.empty());
    EXPECT_GT(string_batch->frames[0].stringcolumns_size(), 0);
    EXPECT_EQ(string_batch->frames[0].stringcolumns(0).name(), pv_string);
    EXPECT_NE(string_batch->frames[0].stringcolumns(0).values(0).find(pv_string), std::string::npos);

    const auto* int_batch = batches_by_source.at(pv_int);
    ASSERT_FALSE(int_batch->frames.empty());
    EXPECT_GT(int_batch->frames[0].int32columns_size(), 0);
    EXPECT_EQ(int_batch->frames[0].int32columns(0).name(), pv_int);

    const auto* waveform_batch = batches_by_source.at(pv_waveform);
    ASSERT_FALSE(waveform_batch->frames.empty());
    EXPECT_GT(waveform_batch->frames[0].doublecolumns_size(), 0);
    EXPECT_EQ(waveform_batch->frames[0].doublecolumns(0).name(), pv_waveform);
    EXPECT_EQ(waveform_batch->frames[0].doublecolumns(0).values_size(), 4);

    const auto* bytes_batch = batches_by_source.at(pv_bytes);
    ASSERT_FALSE(bytes_batch->frames.empty());
    EXPECT_GT(bytes_batch->frames[0].stringcolumns_size(), 0);
    EXPECT_EQ(bytes_batch->frames[0].stringcolumns(0).name(), pv_bytes);
    EXPECT_EQ(bytes_batch->frames[0].stringcolumns(0).values(0).size(), 4);
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
