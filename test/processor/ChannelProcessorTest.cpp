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

#include <metrics/MetricsConfig.h>
#include <processor/ChannelProcessor.h>

#include "../config/test_config_helpers.h"
#include "../common/MldpMetricsTestUtils.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using mldp_pvxs_driver::config::Config;
using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::processor::AlgorithmOutput;
using mldp_pvxs_driver::processor::AlignedSnapshot;
using mldp_pvxs_driver::processor::ChannelProcessor;
using mldp_pvxs_driver::processor::IAlgorithm;
using mldp_pvxs_driver::processor::MLDPChannelProcessorConfig;
using mldp_pvxs_driver::util::bus::EventBatchStruct;
using mldp_pvxs_driver::util::bus::SourceMetadataPayload;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;
using mldp_pvxs_driver::testutil::serializeMetricsText;

namespace {

Config makeProcessorConfig(const std::string& trigger = "any-update",
                           const std::string& extra_sources = "")
{
    return makeConfigFromYaml(
        "name: test-proc\n"
        "sources:\n"
        "  - SRC:A\n" +
        extra_sources +
        "alignment: latest-value\n"
        "trigger: " + trigger + "\n");
}

TimeSeriesPayload makeTimeSeriesPayload(const std::string& root_source_name)
{
    TimeSeriesPayload payload;
    payload.root_source_name = root_source_name;
    payload.frames.push_back({});
    payload.frames.back().timestamps.push_back({10, 5});
    return payload;
}

class CaptureBus final : public mldp_pvxs_driver::util::bus::IDataBus
{
public:
    bool push(EventBatch batch) override
    {
        batches.push_back(std::move(batch));
        return true;
    }

    std::vector<EventBatch> batches;
};

class StubAlgorithm final : public IAlgorithm
{
public:
    void configure(const Config&) override {}

    std::vector<std::string> outputSources() const noexcept override
    {
        return {"VIRTUAL:STUB"};
    }

    std::vector<AlgorithmOutput> compute(const AlignedSnapshot& snapshot) override
    {
        last_snapshot = snapshot;
        ++call_count;

        std::vector<AlgorithmOutput> outputs;
        for (const auto& output_source : configured_outputs)
        {
            TimeSeriesPayload payload;
            payload.root_source_name = output_source;
            payload.end_of_batch_group = true;
            outputs.push_back(AlgorithmOutput{output_source, std::move(payload)});
        }
        return outputs;
    }

    std::string algorithmType() const noexcept override
    {
        return "stub";
    }

    std::vector<std::string> configured_outputs{"VIRTUAL:STUB"};
    AlignedSnapshot          last_snapshot;
    int                      call_count{0};
};

class ThrowingAlgorithm final : public IAlgorithm
{
public:
    void configure(const Config&) override {}

    std::vector<std::string> outputSources() const noexcept override
    {
        return {"VIRTUAL:FAIL"};
    }

    std::vector<AlgorithmOutput> compute(const AlignedSnapshot&) override
    {
        throw std::runtime_error("boom");
    }

    std::string algorithmType() const noexcept override
    {
        return "throwing";
    }
};

std::shared_ptr<BS::light_thread_pool> makePool()
{
    return std::make_shared<BS::light_thread_pool>(1);
}

} // namespace

TEST(ChannelProcessorTest, AnyUpdatePushCausesCompute)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto* algorithm_ptr = algorithm.get();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.reader_name = "reader-a";
    batch.payload = makeTimeSeriesPayload("SRC:A");

    EXPECT_TRUE(processor.push(std::move(batch)));
    pool->wait();
    EXPECT_EQ(algorithm_ptr->call_count, 1);
    ASSERT_EQ(bus->batches.size(), 1u);
}

TEST(ChannelProcessorTest, AnyUpdateOutputReaderName)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");
    ASSERT_TRUE(processor.push(std::move(batch)));
    pool->wait();

    ASSERT_EQ(bus->batches.size(), 1u);
    EXPECT_EQ(bus->batches.front().reader_name, "test-proc");
}

TEST(ChannelProcessorTest, AnyUpdateOutputPayloadType)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");
    ASSERT_TRUE(processor.push(std::move(batch)));
    pool->wait();

    ASSERT_EQ(bus->batches.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<TimeSeriesPayload>(bus->batches.front().payload));
    EXPECT_EQ(std::get<TimeSeriesPayload>(bus->batches.front().payload).root_source_name,
              "VIRTUAL:STUB");
}

TEST(ChannelProcessorTest, NonTSPayloadAcceptedNoCompute)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto* algorithm_ptr = algorithm.get();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = SourceMetadataPayload{.root_source_name = "SRC:A"};

    EXPECT_TRUE(processor.push(std::move(batch)));
    pool->wait();
    EXPECT_EQ(algorithm_ptr->call_count, 0);
    EXPECT_TRUE(bus->batches.empty());
}

TEST(ChannelProcessorTest, AllUpdatedNoComputeUntilBothFresh)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto* algorithm_ptr = algorithm.get();

    ChannelProcessor processor(
        MLDPChannelProcessorConfig(makeProcessorConfig("all-updated", "  - SRC:B\n")),
        std::move(algorithm),
        bus,
        nullptr,
        pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");

    EXPECT_TRUE(processor.push(std::move(batch)));
    pool->wait();
    EXPECT_EQ(algorithm_ptr->call_count, 0);
}

TEST(ChannelProcessorTest, AllUpdatedComputeAfterBothFresh)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto* algorithm_ptr = algorithm.get();

    ChannelProcessor processor(
        MLDPChannelProcessorConfig(makeProcessorConfig("all-updated", "  - SRC:B\n")),
        std::move(algorithm),
        bus,
        nullptr,
        pool);
    processor.start();

    EventBatchStruct batch_a;
    batch_a.payload = makeTimeSeriesPayload("SRC:A");
    EventBatchStruct batch_b;
    batch_b.payload = makeTimeSeriesPayload("SRC:B");

    ASSERT_TRUE(processor.push(std::move(batch_a)));
    ASSERT_TRUE(processor.push(std::move(batch_b)));
    pool->wait();
    EXPECT_EQ(algorithm_ptr->call_count, 1);
    ASSERT_EQ(bus->batches.size(), 1u);
}

TEST(ChannelProcessorTest, AllUpdatedFlagsResetAfterCompute)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto* algorithm_ptr = algorithm.get();

    ChannelProcessor processor(
        MLDPChannelProcessorConfig(makeProcessorConfig("all-updated", "  - SRC:B\n")),
        std::move(algorithm),
        bus,
        nullptr,
        pool);
    processor.start();

    EventBatchStruct batch_a1;
    batch_a1.payload = makeTimeSeriesPayload("SRC:A");
    EventBatchStruct batch_b;
    batch_b.payload = makeTimeSeriesPayload("SRC:B");
    EventBatchStruct batch_a2;
    batch_a2.payload = makeTimeSeriesPayload("SRC:A");

    ASSERT_TRUE(processor.push(std::move(batch_a1)));
    ASSERT_TRUE(processor.push(std::move(batch_b)));
    pool->wait();
    ASSERT_TRUE(processor.push(std::move(batch_a2)));
    pool->wait();

    EXPECT_EQ(algorithm_ptr->call_count, 1);
}

TEST(ChannelProcessorTest, StoppedProcessorPushReturnsFalse)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto* algorithm_ptr = algorithm.get();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);

    // push before start: task is enqueued but running_=false so it silently returns
    EventBatchStruct batch_before_start;
    batch_before_start.payload = makeTimeSeriesPayload("SRC:A");
    EXPECT_TRUE(processor.push(std::move(batch_before_start)));
    pool->wait();
    EXPECT_EQ(algorithm_ptr->call_count, 0);

    processor.start();
    processor.stop();

    EventBatchStruct batch_after_stop;
    batch_after_stop.payload = makeTimeSeriesPayload("SRC:A");
    EXPECT_TRUE(processor.push(std::move(batch_after_stop)));
    pool->wait();
    EXPECT_EQ(algorithm_ptr->call_count, 0);
}

TEST(ChannelProcessorTest, MultipleOutputsAllPushed)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    algorithm->configured_outputs = {"VIRTUAL:ONE", "VIRTUAL:TWO"};

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");

    ASSERT_TRUE(processor.push(std::move(batch)));
    pool->wait();
    ASSERT_EQ(bus->batches.size(), 2u);
    EXPECT_EQ(std::get<TimeSeriesPayload>(bus->batches[0].payload).root_source_name,
              "VIRTUAL:ONE");
    EXPECT_EQ(std::get<TimeSeriesPayload>(bus->batches[1].payload).root_source_name,
              "VIRTUAL:TWO");
}

TEST(ChannelProcessorTest, ComputeExceptionIsSwallowed)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<ThrowingAlgorithm>();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");

    EXPECT_TRUE(processor.push(std::move(batch)));
    pool->wait();
    EXPECT_TRUE(bus->batches.empty());
}

TEST(ChannelProcessorTest, StopDrainsInFlightTask)
{
    // Verify stop() waits for in-flight tasks: push then immediately stop must not crash.
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               nullptr,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");
    processor.push(std::move(batch));
    processor.stop(); // must not crash or use-after-free
}

TEST(ChannelProcessorTest, SuccessfulComputeIncrementsProcessorMetrics)
{
    auto pool = makePool();
    auto bus = std::make_shared<CaptureBus>();
    auto algorithm = std::make_unique<StubAlgorithm>();
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(
        mldp_pvxs_driver::metrics::MetricsConfig{}, "test-controller");

    ChannelProcessor processor(MLDPChannelProcessorConfig(makeProcessorConfig()),
                               std::move(algorithm),
                               bus,
                               metrics,
                               pool);
    processor.start();

    EventBatchStruct batch;
    batch.payload = makeTimeSeriesPayload("SRC:A");
    ASSERT_TRUE(processor.push(std::move(batch)));
    pool->wait();

    const auto text = serializeMetricsText(*metrics);
    EXPECT_NE(text.find("mldp_pvxs_driver_processor_fire_total{controller=\"test-controller\",processor=\"test-proc\"} 1"),
              std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_processor_buffer_depth{controller=\"test-controller\",processor=\"test-proc\"} 1"),
              std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_processor_compute_latency_us_count{controller=\"test-controller\",processor=\"test-proc\"} 1"),
              std::string::npos);
}
