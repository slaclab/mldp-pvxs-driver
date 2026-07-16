#include <gtest/gtest.h>

#include <enricher/EnricherRegistry.h>
#include <util/bus/IDataBus.h>

#include "config/test_config_helpers.h"

namespace mldp_pvxs_driver::enricher {
namespace {

using config::makeConfigFromYaml;
using util::bus::DataBatch;
using util::bus::DataColumn;
using util::bus::EventBatchStruct;
using util::bus::TimeSeriesPayload;

EventBatchStruct timeSeriesBatch(std::string column = "PV:ONE")
{
    DataColumn data_column;
    data_column.name = std::move(column);
    data_column.values = std::vector<double>{1.0};
    DataBatch frame;
    frame.timestamps.push_back({.epoch_seconds = 1, .nanoseconds = 1000000000});
    frame.columns.push_back(std::move(data_column));
    return EventBatchStruct{.payload = TimeSeriesPayload{.frames = {std::move(frame)}}};
}

TEST(EnricherRegistryTest, ResolvesOrderedSharedInstancesAndAppliesBuiltins)
{
    const auto config = makeConfigFromYaml(R"(
enrichers:
  metadata:
    type: static-metadata
    metadata: {run: forty-two}
  attributes:
    type: column-attributes
    column-pattern: 'PV:*'
    attributes: {unit: A}
  timestamps:
    type: timestamp-clamp
writer:
  mldp:
    - name: writer-a
      enrichers: [metadata, attributes, timestamps]
    - name: writer-b
      enrichers: [metadata]
)");

    EnricherRegistry registry(config);
    const auto writers = config.subConfig("writer").front().subConfig("mldp");
    const auto first = registry.resolve(writers[0]);
    const auto second = registry.resolve(writers[1]);
    ASSERT_EQ(3U, first.size());
    ASSERT_EQ(1U, second.size());
    EXPECT_EQ(first[0].get(), second[0].get());

    auto batch = timeSeriesBatch();
    for (const auto& enricher : first)
        ASSERT_TRUE(enricher->run(batch));
    EXPECT_EQ("forty-two", batch.metadata.at("run"));
    const auto& frame = std::get<TimeSeriesPayload>(batch.payload).frames.front();
    EXPECT_EQ("A", frame.columns.front().metadata.at("unit"));
    EXPECT_EQ(999999999U, frame.timestamps.front().nanoseconds);
}

TEST(EnricherRegistryTest, RejectsUnknownAndDuplicateWriterReferences)
{
    const auto unknown = makeConfigFromYaml(R"(
enrichers: {one: {type: timestamp-clamp}}
writer: {mldp: [{enrichers: [missing]}]}
)");
    EnricherRegistry registry(unknown);
    EXPECT_THROW(registry.resolve(unknown.subConfig("writer").front().subConfig("mldp").front()), std::runtime_error);

    const auto duplicate = makeConfigFromYaml(R"(
enrichers: {one: {type: timestamp-clamp}}
writer: {mldp: [{enrichers: [one, one]}]}
)");
    EnricherRegistry duplicate_registry(duplicate);
    EXPECT_THROW(duplicate_registry.resolve(duplicate.subConfig("writer").front().subConfig("mldp").front()), std::runtime_error);
}

TEST(EnricherRegistryTest, ShardSlotIsStableFormattedAndPreservesExisting)
{
    const auto config = makeConfigFromYaml(R"(
enrichers: {slots: {type: shard-slot, num-shards: 6}}
writer: {mldp: [{enrichers: [slots]}]}
)");
    EnricherRegistry registry(config);
    const auto writer = config.subConfig("writer").front().subConfig("mldp").front();
    const auto slots = registry.resolve(writer);
    auto first = timeSeriesBatch("PV:ONE");
    auto second = timeSeriesBatch("PV:ONE");
    ASSERT_TRUE(slots.front()->run(first));
    ASSERT_TRUE(slots.front()->run(second));
    const auto first_slot = std::get<TimeSeriesPayload>(first.payload).frames.front().columns.front().metadata.at("shardSlot");
    EXPECT_EQ(5U, first_slot.size());
    EXPECT_EQ(first_slot, std::get<TimeSeriesPayload>(second.payload).frames.front().columns.front().metadata.at("shardSlot"));
    auto preserved = timeSeriesBatch("PV:TWO");
    std::get<TimeSeriesPayload>(preserved.payload).frames.front().columns.front().metadata["shardSlot"] = "12345";
    ASSERT_TRUE(slots.front()->run(preserved));
    EXPECT_EQ("12345", std::get<TimeSeriesPayload>(preserved.payload).frames.front().columns.front().metadata.at("shardSlot"));
}

} // namespace
} // namespace mldp_pvxs_driver::enricher
