//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#ifdef BUILD_ECHO_PROCESSOR

#include <gtest/gtest.h>

#include <processor/impl/EchoAlgorithm.h>

#include "../config/test_config_helpers.h"

#include <string>
#include <vector>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::processor::AlignedSnapshot;
using mldp_pvxs_driver::processor::EchoAlgorithm;
using mldp_pvxs_driver::util::bus::DataBatch;
using mldp_pvxs_driver::util::bus::DataColumn;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;

namespace {

DataBatch makeBatch(double value)
{
    DataBatch batch;
    batch.timestamps.push_back({12, 34});
    batch.columns.push_back(DataColumn{"value", std::vector<double>{value}});
    return batch;
}

AlignedSnapshot makeSnapshot(std::initializer_list<std::pair<const std::string, double>> values)
{
    AlignedSnapshot snapshot;
    snapshot.reference_time = {20, 99};
    for (const auto& [source, value] : values)
    {
        snapshot.channels.emplace(source, makeBatch(value));
    }
    return snapshot;
}

} // namespace

TEST(EchoAlgorithmTest, ExplicitOutputSource)
{
    EchoAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: echo
sources:
  - BPM:X
output-source: VIRTUAL:ECHO
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"BPM:X", 7.0}}));
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().output_source, "VIRTUAL:ECHO");

    const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
    EXPECT_EQ(payload.root_source_name, "VIRTUAL:ECHO");
}

TEST(EchoAlgorithmTest, AutoDerivedOutputSource)
{
    EchoAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: echo
sources:
  - BPM:X
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"BPM:X", 7.0}}));
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front().output_source, "BPM:X-echo");

    const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
    EXPECT_EQ(payload.root_source_name, "BPM:X-echo");
}

TEST(EchoAlgorithmTest, EmptySnapshotReturnsEmpty)
{
    EchoAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: echo
sources:
  - BPM:X
)yaml"));

    const auto outputs = algorithm.compute(AlignedSnapshot{});
    EXPECT_TRUE(outputs.empty());
}

TEST(EchoAlgorithmTest, PassesThroughValues)
{
    EchoAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: echo
sources:
  - BPM:X
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"BPM:X", 42.0}}));
    ASSERT_EQ(outputs.size(), 1u);

    const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
    ASSERT_EQ(payload.frames.size(), 1u);
    ASSERT_EQ(payload.frames.front().columns.size(), 1u);

    const auto& values = std::get<std::vector<double>>(payload.frames.front().columns.front().values);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_DOUBLE_EQ(values.front(), 42.0);
}

#endif // BUILD_ECHO_PROCESSOR
