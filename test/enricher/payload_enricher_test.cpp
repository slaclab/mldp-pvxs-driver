//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////
#include <gtest/gtest.h>

#include <enricher/BuiltinEnrichers.h>

#include "config/test_config_helpers.h"

namespace mldp_pvxs_driver::enricher {
namespace {

using config::makeConfigFromYaml;
using util::bus::ConfigurationActivationPayload;
using util::bus::ConfigurationPayload;
using util::bus::DataBatch;
using util::bus::DataColumn;
using util::bus::EventBatchStruct;
using util::bus::SourceMetadataPayload;
using util::bus::TimeSeriesPayload;

std::vector<EventBatchStruct> allPayloadTypes()
{
    DataColumn column{.name = "PV:ONE", .values = std::vector<double>{1.0}};
    DataBatch frame{.timestamps = {{.epoch_seconds = 1, .nanoseconds = 1000000000}}, .columns = {std::move(column)}};
    return {
        EventBatchStruct{.payload = TimeSeriesPayload{.root_source_name = "TS", .frames = {std::move(frame)}}},
        EventBatchStruct{.payload = SourceMetadataPayload{.root_source_name = "META"}},
        EventBatchStruct{.payload = ConfigurationPayload{.root_source_name = "CONFIG", .configuration_name = "config"}},
        EventBatchStruct{.payload = ConfigurationActivationPayload{.configuration_name = "activation"}},
    };
}

TEST(PayloadEnricherTest, StaticMetadataSupportsEveryPayloadType)
{
    StaticMetadataEnricher enricher(makeConfigFromYaml("metadata: {run: run-42, owner: test}"));
    for (auto& batch : allPayloadTypes())
    {
        batch.metadata["run"] = "old";
        ASSERT_TRUE(enricher.run(batch));
        EXPECT_EQ("run-42", batch.metadata.at("run"));
        EXPECT_EQ("test", batch.metadata.at("owner"));
    }
}

TEST(PayloadEnricherTest, TimeSeriesOnlyEnrichersLeaveOtherPayloadsUntouched)
{
    ColumnAttributesEnricher attributes(makeConfigFromYaml("column-pattern: 'PV:*'\nattributes: {unit: A}"));
    TimestampClampEnricher clamp({});
    auto batches = allPayloadTypes();
    for (auto& batch : batches)
    {
        ASSERT_TRUE(attributes.run(batch));
        ASSERT_TRUE(clamp.run(batch));
    }

    const auto& frame = std::get<TimeSeriesPayload>(batches.front().payload).frames.front();
    EXPECT_EQ("A", frame.columns.front().metadata.at("unit"));
    EXPECT_EQ(999999999U, frame.timestamps.front().nanoseconds);
    for (std::size_t index = 1; index < batches.size(); ++index)
        EXPECT_TRUE(batches[index].metadata.empty());
}

} // namespace
} // namespace mldp_pvxs_driver::enricher
