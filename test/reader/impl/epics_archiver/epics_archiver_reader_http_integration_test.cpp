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

#include <metrics/Metrics.h>
#include <prometheus/labels.h>
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
using mldp_pvxs_driver::util::bus::DataBatch;
using mldp_pvxs_driver::util::bus::DataColumn;
using mldp_pvxs_driver::util::bus::IDataBus;
// Backward compatibility alias
using MockEventBusPush = mldp_pvxs_driver::test::mock::MockDataBus;
using mldp_pvxs_driver::util::bus::asTimeSeries;
using mldp_pvxs_driver::util::bus::getRootSourceName;

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
    ASSERT_EQ(asTimeSeries(batches[0]).frames.size(), 4u);
    EXPECT_EQ(getRootSourceName(batches[0]), "TEST:PV:DOUBLE");

    uint64_t prev_epoch = 0;
    uint64_t prev_nano = 0;
    for (size_t i = 0; i < asTimeSeries(batches[0]).frames.size(); ++i)
    {
        const auto& frame = asTimeSeries(batches[0]).frames[i];
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
        batches_by_source.emplace(getRootSourceName(batch), &batch);
    }

    ASSERT_TRUE(batches_by_source.count(pv_string));
    ASSERT_TRUE(batches_by_source.count(pv_int));
    ASSERT_TRUE(batches_by_source.count(pv_waveform));
    ASSERT_TRUE(batches_by_source.count(pv_bytes));

    const auto* string_batch = batches_by_source.at(pv_string);
    ASSERT_FALSE(asTimeSeries(*string_batch).frames.empty());
    {
        const auto& col = asTimeSeries(*string_batch).frames[0].columns.at(0);
        EXPECT_EQ(col.name, pv_string);
        const auto& sv = getStrings(col);
        ASSERT_FALSE(sv.empty());
        EXPECT_NE(sv[0].find(pv_string), std::string::npos);
    }

    const auto* int_batch = batches_by_source.at(pv_int);
    ASSERT_FALSE(asTimeSeries(*int_batch).frames.empty());
    {
        const auto& col = asTimeSeries(*int_batch).frames[0].columns.at(0);
        EXPECT_EQ(col.name, pv_int);
        EXPECT_NO_THROW(getInt32s(col));
    }

    const auto* waveform_batch = batches_by_source.at(pv_waveform);
    ASSERT_FALSE(asTimeSeries(*waveform_batch).frames.empty());
    {
        const auto& col     = asTimeSeries(*waveform_batch).frames[0].columns.at(0);
        const auto& arrays  = getDoubleArrays(col);
        EXPECT_EQ(col.name, pv_waveform);
        ASSERT_FALSE(arrays.empty());
        EXPECT_EQ(arrays[0].size(), 4u);
    }

    const auto* bytes_batch = batches_by_source.at(pv_bytes);
    ASSERT_FALSE(asTimeSeries(*bytes_batch).frames.empty());
    {
        const auto& col   = asTimeSeries(*bytes_batch).frames[0].columns.at(0);
        EXPECT_EQ(col.name, pv_bytes);
        const auto& blobs = getBlobs(col);
        ASSERT_FALSE(blobs.empty());
        EXPECT_EQ(blobs[0].size(), 4u);
    }
}

// Verifies configured sample-count batching and the optional 'to' query parameter.
TEST(EpicsArchiverReaderHttpIntegrationTest, SplitsPublishedBatchesByConfiguredSampleCount)
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
        pv-samples-per-batch: 4
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 5u, std::chrono::seconds(2)));
    EXPECT_EQ(reader->name(), "archiver-http-batch-split");

    const auto req = server.lastRequest();
    ASSERT_TRUE(req.to.has_value());
    EXPECT_EQ(*req.to, "2026-02-25T08:00:05.000Z");

    const auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 5u);

    uint64_t prev_epoch = 0u;
    uint64_t prev_nano = 0u;
    bool     first = true;

    for (const auto& batch : batches)
    {
        EXPECT_EQ(getRootSourceName(batch), "TEST:PV:DOUBLE");
        ASSERT_EQ(asTimeSeries(batch).frames.size(), 1u);

        const auto& frame = asTimeSeries(batch).frames[0];
        ASSERT_EQ(frame.timestamps.size(), 4u);
        for (const auto& timestamp : frame.timestamps)
        {
            if (!first)
            {
                const bool nondecreasing =
                    (timestamp.epoch_seconds > prev_epoch) ||
                    (timestamp.epoch_seconds == prev_epoch && timestamp.nanoseconds >= prev_nano);
                EXPECT_TRUE(nondecreasing);
            }
            prev_epoch = timestamp.epoch_seconds;
            prev_nano = timestamp.nanoseconds;
            first = false;
        }
    }
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

// Verify reader-level metadata and per-PV metadata overrides are merged into EventBatch.metadata.
// Reader config uses YAML key "metadata" for both reader-level and per-PV blocks.
// Per-PV keys win over reader-level keys on conflict.
TEST(EpicsArchiverReaderHttpIntegrationTest, StaticAndPerPvMetadataMergedIntoEventBatch)
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
        name: archiver-meta-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        metadata:
          facility: lcls
          subsystem: bpms
        pvs:
          - name: "TEST:PV:DOUBLE"
            metadata:
              signal_type: scalar
              subsystem: override_bpms
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(2)));

    const auto batches = bus->snapshot();
    ASSERT_FALSE(batches.empty());

    // Verify metadata on the batch for TEST:PV:DOUBLE.
    bool found = false;
    for (const auto& batch : batches)
    {
        if (getRootSourceName(batch) != "TEST:PV:DOUBLE")
            continue;
        found = true;
        // Reader-level key not overridden by per-PV.
        EXPECT_EQ(batch.metadata.count("facility"), 1u);
        EXPECT_EQ(batch.metadata.at("facility"), "lcls");
        // Per-PV key not present at reader level.
        EXPECT_EQ(batch.metadata.count("signal_type"), 1u);
        EXPECT_EQ(batch.metadata.at("signal_type"), "scalar");
        // Per-PV key overrides reader-level key with same name.
        EXPECT_EQ(batch.metadata.count("subsystem"), 1u);
        EXPECT_EQ(batch.metadata.at("subsystem"), "override_bpms");
    }
    EXPECT_TRUE(found) << "No batch with root_source=TEST:PV:DOUBLE received";
}

TEST(EpicsArchiverReaderHttpIntegrationTest, ReaderDataBytesMetricsIncrementedAfterFetch)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second   = 4;
    gen_cfg.max_events_per_second   = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed             = 42;
    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(
        mldp_pvxs_driver::metrics::MetricsConfig());
    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: archiver-bytes-metrics-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, metrics, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));

    const prometheus::Labels source_tag{{"source", "TEST:PV:DOUBLE"}};
    EXPECT_GT(metrics->readerDataBytesTotal(source_tag), 0.0)
        << "readerDataBytesTotal not incremented after archiver fetch";
    EXPECT_GT(metrics->readerDataBytesPerSecond(source_tag), 0.0)
        << "readerDataBytesPerSecond not set after archiver fetch";
}

// ============================================================================
// pv-samples-per-batch batching tests
// ============================================================================

// Verifies that exactly pv-samples-per-batch samples trigger one bus submission.
TEST(EpicsArchiverReaderHttpIntegrationTest, SamplesReachingPvSamplesPerBatchTriggerSubmission)
{
    // Generate exactly 4 samples (1 second at 4 eps).
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 1001;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    // pv-samples-per-batch=4 matches the total sample count → exactly one push.
    const std::string yaml = std::string(R"(
        name: batch-trigger-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 4
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));

    // Allow the reader to process and submit.
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 1u, std::chrono::seconds(2)));

    const auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(getRootSourceName(batches[0]), "TEST:PV:DOUBLE");
    // One merged DataBatch with 4 aligned timestamp+value rows.
    ASSERT_EQ(asTimeSeries(batches[0]).frames.size(), 1u);
    EXPECT_EQ(asTimeSeries(batches[0]).frames[0].timestamps.size(), 4u);
}

// Verifies that samples from two PVs are accumulated into separate pending batches.
TEST(EpicsArchiverReaderHttpIntegrationTest, SamplesFromDifferentPvsRemainSeparated)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 2002;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    // Two PVs, each producing 4 samples; batch size 4 → two separate submissions.
    const std::string yaml = std::string(R"(
        name: two-pv-separation-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 4
        pvs:
          - name: "TEST:PV:DOUBLE"
          - name: "TEST:PV2:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(server.waitForRequestCount(2u, std::chrono::seconds(3)));
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 2u, std::chrono::seconds(3)));

    const auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 2u);

    std::set<std::string> sources;
    for (const auto& b : batches)
        sources.insert(getRootSourceName(b));

    EXPECT_EQ(sources, (std::set<std::string>{"TEST:PV:DOUBLE", "TEST:PV2:DOUBLE"}));
}

// Verifies that 2*N samples for one PV produce two separate bus submissions.
TEST(EpicsArchiverReaderHttpIntegrationTest, MultipleFullBatchesFromOnePvSubmittedCorrectly)
{
    // 8 samples at 8 eps over 1 second; batch size 4 → two pushes.
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 8;
    gen_cfg.max_events_per_second = 8;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 3003;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: double-batch-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 4
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 2u, std::chrono::seconds(3)));

    const auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 2u);

    size_t total_samples = 0;
    for (const auto& b : batches)
    {
        EXPECT_EQ(getRootSourceName(b), "TEST:PV:DOUBLE");
        // Each push contains one merged DataBatch.
        ASSERT_EQ(asTimeSeries(b).frames.size(), 1u);
        total_samples += asTimeSeries(b).frames[0].timestamps.size();
    }
    EXPECT_EQ(total_samples, 8u);
}

// Verifies that shutdown flushes remaining pending samples when batch-flush-interval-ms is set.
// batch-flush-interval-ms enables timed flushing; shutdown drains whatever is still pending.
TEST(EpicsArchiverReaderHttpIntegrationTest, ShutdownFlushesRemainingPendingSamples)
{
    // 4 samples; batch size 8 (never full); flush interval enabled → shutdown must flush.
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 4004;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    // batch-flush-interval-ms set to a large value so the timed flush does not fire
    // during the fetch; shutdown flush must drain the 4 pending samples.
    const std::string yaml = std::string(R"(
        name: shutdown-flush-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 8
        batch-flush-interval-ms: 600000
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));

    // Destroy the reader; shutdown path must flush the 4 pending samples.
    reader.reset();

    const auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 1u);
    // One merged DataBatch with 4 aligned timestamp+value rows.
    ASSERT_EQ(asTimeSeries(batches[0]).frames.size(), 1u);
    EXPECT_EQ(asTimeSeries(batches[0]).frames[0].timestamps.size(), 4u);
}

// Verifies that incomplete batches are NOT flushed at shutdown when batch-flush-interval-ms is disabled.
TEST(EpicsArchiverReaderHttpIntegrationTest, ShutdownDoesNotFlushWhenFlushIntervalDisabled)
{
    // 4 samples; batch size 8 (never full); no flush interval → samples dropped at shutdown.
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 7007;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: no-flush-on-shutdown-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 8
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));
    reader.reset();

    // No batch-flush-interval-ms → incomplete batch not pushed.
    EXPECT_EQ(bus->snapshot().size(), 0u);
}

// Verifies that batch-flush-interval-ms flushes an incomplete pending batch after the interval.
TEST(EpicsArchiverReaderHttpIntegrationTest, FlushIntervalSubmitsIncompleteBatches)
{
    // 4 samples; batch size 100 (never full); flush interval 10 ms.
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 5005;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    // batch-flush-interval-ms is small enough to fire after the fetch completes.
    const std::string yaml = std::string(R"(
        name: flush-interval-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 100
        batch-flush-interval-ms: 10
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));

    // The flush is triggered after fetchConfiguredPVs returns; 10 ms is long past.
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 1u, std::chrono::seconds(2)));

    const auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 1u);
    // One merged DataBatch with 4 aligned timestamp+value rows.
    ASSERT_EQ(asTimeSeries(batches[0]).frames.size(), 1u);
    EXPECT_EQ(asTimeSeries(batches[0]).frames[0].timestamps.size(), 4u);
}

// Verifies that one flush submits incomplete batches for multiple PVs in a single pass.
TEST(EpicsArchiverReaderHttpIntegrationTest, OneFlushSubmitsIncompleteBatchesForMultiplePvs)
{
    MockArchiverPbHttpServer::GenerationConfig gen_cfg;
    gen_cfg.min_events_per_second = 4;
    gen_cfg.max_events_per_second = 4;
    gen_cfg.open_ended_duration_sec = 1;
    gen_cfg.random_seed = 6006;

    MockArchiverPbHttpServer server(gen_cfg);
    server.start();
    ASSERT_GT(server.port(), 0);

    auto bus = std::make_shared<MockEventBusPush>();

    const std::string yaml = std::string(R"(
        name: multi-pv-flush-test
        hostname: ")") + server.baseUrl() +
                             R"("
        start-date: "2026-02-25T08:00:00.000Z"
        pv-samples-per-batch: 100
        batch-flush-interval-ms: 10
        pvs:
          - name: "TEST:PV:DOUBLE"
          - name: "TEST:PV2:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader     = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
    ASSERT_TRUE(server.waitForRequestCount(2u, std::chrono::seconds(3)));
    ASSERT_TRUE(waitForMockRequestStartAndCompletion(server, std::chrono::seconds(3)));
    ASSERT_TRUE(waitForAtLeastPublishedBatches(*bus, 2u, std::chrono::seconds(3)));

    const auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 2u);

    std::set<std::string> sources;
    for (const auto& b : batches)
        sources.insert(getRootSourceName(b));

    EXPECT_EQ(sources, (std::set<std::string>{"TEST:PV:DOUBLE", "TEST:PV2:DOUBLE"}));
}

} // namespace
