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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace {

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_archiver::EpicsArchiverReader;
using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;
using mldp_pvxs_driver::test::mock::waitForAtLeastPublishedBatches;
using mldp_pvxs_driver::test::mock::waitForMockRequestStart;
using mldp_pvxs_driver::test::mock::waitForMockRequestStartAndCompletion;
using mldp_pvxs_driver::util::bus::DataBatch;
using mldp_pvxs_driver::util::bus::DataColumn;
using mldp_pvxs_driver::util::bus::IDataBus;
// Backward compatibility alias
using MockEventBusPush = mldp_pvxs_driver::test::mock::MockDataBus;

// Helper: get first DataColumn with a vector<double> from a DataBatch.
auto findDoubleCol   = [](const DataBatch& b, std::size_t idx) -> const DataColumn& { return b.columns.at(idx); };
auto getDoubles      = [](const DataColumn& c) -> const std::vector<double>& { return std::get<std::vector<double>>(c.values); };
auto getStrings      = [](const DataColumn& c) -> const std::vector<std::string>& { return std::get<std::vector<std::string>>(c.values); };
auto getInt32s       = [](const DataColumn& c) -> const std::vector<int32_t>& { return std::get<std::vector<int32_t>>(c.values); };
auto getDoubleArrays = [](const DataColumn& c) -> const std::vector<std::vector<double>>& { return std::get<std::vector<std::vector<double>>>(c.values); };
auto getBlobs        = [](const DataColumn& c) -> const std::vector<std::vector<uint8_t>>& { return std::get<std::vector<std::vector<uint8_t>>>(c.values); };

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
        ASSERT_FALSE(frame.columns.empty());
        const auto& col     = findDoubleCol(frame, 0);
        const auto& doubles = getDoubles(col);
        ASSERT_FALSE(doubles.empty());
        EXPECT_EQ(col.name, "TEST:PV:DOUBLE");
        EXPECT_GE(doubles[0], gen_cfg.min_value);
        EXPECT_LE(doubles[0], gen_cfg.max_value);
        ASSERT_FALSE(frame.timestamps.empty());
        const auto epoch = frame.timestamps[0].epoch_seconds;
        const auto nano  = frame.timestamps[0].nanoseconds;
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
    {
        const auto& col = string_batch->frames[0].columns.at(0);
        EXPECT_EQ(col.name, pv_string);
        const auto& sv = getStrings(col);
        ASSERT_FALSE(sv.empty());
        EXPECT_NE(sv[0].find(pv_string), std::string::npos);
    }

    const auto* int_batch = batches_by_source.at(pv_int);
    ASSERT_FALSE(int_batch->frames.empty());
    {
        const auto& col = int_batch->frames[0].columns.at(0);
        EXPECT_EQ(col.name, pv_int);
        EXPECT_NO_THROW(getInt32s(col));
    }

    const auto* waveform_batch = batches_by_source.at(pv_waveform);
    ASSERT_FALSE(waveform_batch->frames.empty());
    {
        const auto& col     = waveform_batch->frames[0].columns.at(0);
        const auto& arrays  = getDoubleArrays(col);
        EXPECT_EQ(col.name, pv_waveform);
        ASSERT_FALSE(arrays.empty());
        EXPECT_EQ(arrays[0].size(), 4u);
    }

    const auto* bytes_batch = batches_by_source.at(pv_bytes);
    ASSERT_FALSE(bytes_batch->frames.empty());
    {
        const auto& col   = bytes_batch->frames[0].columns.at(0);
        EXPECT_EQ(col.name, pv_bytes);
        const auto& blobs = getBlobs(col);
        ASSERT_FALSE(blobs.empty());
        EXPECT_EQ(blobs[0].size(), 4u);
    }
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
        ASSERT_FALSE(first_frame.timestamps.empty());
        ASSERT_FALSE(last_frame.timestamps.empty());
        const auto first_epoch_s = first_frame.timestamps[0].epoch_seconds;
        const auto first_nano_s  = first_frame.timestamps[0].nanoseconds;
        const auto last_epoch_s  = last_frame.timestamps[0].epoch_seconds;
        const auto last_nano_s   = last_frame.timestamps[0].nanoseconds;

        const uint64_t first_ns = first_epoch_s * 1'000'000'000ULL + first_nano_s;
        const uint64_t last_ns  = last_epoch_s * 1'000'000'000ULL + last_nano_s;
        EXPECT_LE(last_ns - first_ns, 1'000'000'000ULL);

        for (const auto& frame : batch.frames)
        {
            ASSERT_FALSE(frame.timestamps.empty());
            total_events++;
            const auto ts_epoch = frame.timestamps[0].epoch_seconds;
            const auto ts_nano  = frame.timestamps[0].nanoseconds;
            if (!first)
            {
                const bool nondecreasing =
                    (ts_epoch > prev_epoch) || (ts_epoch == prev_epoch && ts_nano >= prev_nano);
                EXPECT_TRUE(nondecreasing);
            }
            prev_epoch = ts_epoch;
            prev_nano = ts_nano;
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
