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
#include <util/bus/IEventBusPush.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_archiver::EpicsArchiverReader;
using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;

class MockEventBusPush final : public mldp_pvxs_driver::util::bus::IEventBusPush
{
public:
    using EventBatch = mldp_pvxs_driver::util::bus::IEventBusPush::EventBatch;

    bool push(EventBatch batch) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        received.emplace_back(std::move(batch));
        return true;
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
        start_date: "2026-02-25T08:00:00.000Z"
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);

    EXPECT_EQ(reader->name(), "archiver-http-test");

    const auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].values.size(), 1u);
    EXPECT_EQ(batches[0].root_source, "TEST:PV:DOUBLE");

    const auto it = batches[0].values.find("TEST:PV:DOUBLE");
    ASSERT_NE(it, batches[0].values.end());
    ASSERT_EQ(it->second.size(), 4u);

    uint64_t prev_epoch = 0;
    uint64_t prev_nano = 0;
    for (size_t i = 0; i < it->second.size(); ++i)
    {
        ASSERT_TRUE(it->second[i]);
        EXPECT_EQ(it->second[i]->data_value.value_case(), DataValue::kDoubleValue);
        EXPECT_GE(it->second[i]->data_value.doublevalue(), gen_cfg.min_value);
        EXPECT_LE(it->second[i]->data_value.doublevalue(), gen_cfg.max_value);
        EXPECT_GT(it->second[i]->epoch_seconds, 0u);
        EXPECT_LT(it->second[i]->nanoseconds, 1'000'000'000u);
        if (i > 0)
        {
            const bool nondecreasing =
                (it->second[i]->epoch_seconds > prev_epoch) || (it->second[i]->epoch_seconds == prev_epoch && it->second[i]->nanoseconds >= prev_nano);
            EXPECT_TRUE(nondecreasing);
        }
        prev_epoch = it->second[i]->epoch_seconds;
        prev_nano = it->second[i]->nanoseconds;
    }

    const auto req = server.lastRequest();
    ASSERT_TRUE(req.pv.has_value());
    ASSERT_TRUE(req.from.has_value());
    EXPECT_EQ(*req.pv, "TEST:PV:DOUBLE");
    EXPECT_EQ(*req.from, "2026-02-25T08:00:00.000Z");
    EXPECT_FALSE(req.to.has_value());
}

// Verifies the reader includes the optional end_date as the archiver 'to' query parameter.
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
        start_date: "2026-02-25T08:00:00.000Z"
        end_date: "2026-02-25T08:00:02.000Z"
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
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
        start_date: "2026-02-25T08:00:00.000Z"
        end_date: "2026-02-25T08:00:05.000Z"
        batch_duration_sec: 1
        pvs:
          - name: "TEST:PV:DOUBLE"
    )";

    auto reader_cfg = makeConfigFromYaml(yaml);
    auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, reader_cfg);
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
        const auto it = batch.values.find("TEST:PV:DOUBLE");
        ASSERT_NE(it, batch.values.end());
        ASSERT_FALSE(it->second.empty());

        const auto& first_ev = it->second.front();
        const auto& last_ev = it->second.back();
        ASSERT_TRUE(first_ev);
        ASSERT_TRUE(last_ev);

        const uint64_t first_ns = first_ev->epoch_seconds * 1'000'000'000ULL + first_ev->nanoseconds;
        const uint64_t last_ns = last_ev->epoch_seconds * 1'000'000'000ULL + last_ev->nanoseconds;
        EXPECT_LE(last_ns - first_ns, 1'000'000'000ULL);

        for (const auto& ev : it->second)
        {
            ASSERT_TRUE(ev);
            total_events++;
            if (!first)
            {
                const bool nondecreasing =
                    (ev->epoch_seconds > prev_epoch) || (ev->epoch_seconds == prev_epoch && ev->nanoseconds >= prev_nano);
                EXPECT_TRUE(nondecreasing);
            }
            prev_epoch = ev->epoch_seconds;
            prev_nano = ev->nanoseconds;
            first = false;
        }
    }

    EXPECT_EQ(total_events, 20u);
}

} // namespace
