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

#include <processor/impl/LinearTransformAlgorithm.h>

#include "../config/test_config_helpers.h"

#include <string>
#include <vector>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::processor::AlignedSnapshot;
using mldp_pvxs_driver::processor::LinearTransformAlgorithm;
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

double outputValue(const std::vector<mldp_pvxs_driver::processor::AlgorithmOutput>& outputs)
{
    const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
    return std::get<std::vector<double>>(payload.frames.front().columns.front().values).front();
}

} // namespace

TEST(LinearTransformAlgorithmTest, ComputesTwoSourceLinearCombination)
{
    LinearTransformAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: linear
sources:
  - SRC:A
  - SRC:B
output-source: VIRTUAL:LINEAR
coefficients:
  - 2.0
  - -1.0
bias: 0.5
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"SRC:A", 3.0}, {"SRC:B", 1.0}}));
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_DOUBLE_EQ(outputValue(outputs), 5.5);
}

TEST(LinearTransformAlgorithmTest, SingleSource)
{
    LinearTransformAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: linear
sources:
  - SRC:A
output-source: VIRTUAL:LINEAR
coefficients:
  - 1.0
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"SRC:A", 7.25}}));
    EXPECT_DOUBLE_EQ(outputValue(outputs), 7.25);
}

TEST(LinearTransformAlgorithmTest, DefaultBias)
{
    LinearTransformAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: linear
sources:
  - SRC:A
  - SRC:B
output-source: VIRTUAL:LINEAR
coefficients:
  - 1.5
  - 0.5
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"SRC:A", 4.0}, {"SRC:B", 2.0}}));
    EXPECT_DOUBLE_EQ(outputValue(outputs), 7.0);
}

TEST(LinearTransformAlgorithmTest, OutputSourceSetCorrectly)
{
    LinearTransformAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: linear
sources:
  - SRC:A
output-source: VIRTUAL:TARGET
coefficients:
  - 1.0
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"SRC:A", 1.0}}));
    const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
    EXPECT_EQ(outputs.front().output_source, "VIRTUAL:TARGET");
    EXPECT_EQ(payload.root_source_name, "VIRTUAL:TARGET");
}

TEST(LinearTransformAlgorithmTest, OutputColumnNameInBatch)
{
    LinearTransformAlgorithm algorithm;
    algorithm.configure(makeConfigFromYaml(R"yaml(
name: linear
sources:
  - SRC:A
output-source: VIRTUAL:TARGET
coefficients:
  - 1.0
output-column: custom
)yaml"));

    const auto outputs = algorithm.compute(makeSnapshot({{"SRC:A", 1.0}}));
    const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
    ASSERT_EQ(payload.frames.size(), 1u);
    ASSERT_EQ(payload.frames.front().columns.size(), 1u);
    EXPECT_EQ(payload.frames.front().columns.front().name, "custom");
}

TEST(LinearTransformAlgorithmTest, MissingOutputSourceThrows)
{
    LinearTransformAlgorithm algorithm;
    EXPECT_THROW(algorithm.configure(makeConfigFromYaml(R"yaml(
name: linear
sources:
  - SRC:A
coefficients:
  - 1.0
)yaml")), std::runtime_error);
}
