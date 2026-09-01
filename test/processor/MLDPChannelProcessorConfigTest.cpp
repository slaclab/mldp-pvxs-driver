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

#include <processor/MLDPChannelProcessorConfig.h>

#include <vector>

#include "../config/test_config_helpers.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::processor::AlignmentPolicy;
using mldp_pvxs_driver::processor::MLDPChannelProcessorConfig;
using mldp_pvxs_driver::processor::TriggerPolicy;

namespace {

std::string minimalYaml()
{
    return R"yaml(
name: processor-a
sources:
  - source:a
  - source:b
)yaml";
}

} // namespace

TEST(MLDPChannelProcessorConfigTest, ParsesMinimalValid)
{
    const auto cfg = makeConfigFromYaml(minimalYaml());

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_EQ(config.name(), "processor-a");
    ASSERT_EQ(config.sources().size(), 2u);
    EXPECT_EQ(config.sources()[0], "source:a");
    EXPECT_EQ(config.sources()[1], "source:b");
    EXPECT_EQ(config.alignment(), AlignmentPolicy::LatestValue);
    EXPECT_EQ(config.trigger(), TriggerPolicy::AnyUpdate);
    EXPECT_DOUBLE_EQ(config.triggerIntervalSec(), 0.0);
    EXPECT_EQ(config.maxBufferDepth(), 0u);
}

TEST(MLDPChannelProcessorConfigTest, ParsesAllAlignment)
{
    struct Case
    {
        const char* yaml_value;
        AlignmentPolicy expected;
    };

    const std::vector<Case> cases{{"latest-value", AlignmentPolicy::LatestValue},
                                  {"all-updated", AlignmentPolicy::AllUpdated},
                                  {"interpolate", AlignmentPolicy::Interpolate}};

    for (const auto& test_case : cases)
    {
        const auto cfg = makeConfigFromYaml(std::string(R"yaml(
name: p
sources:
  - s1
alignment: )yaml") + test_case.yaml_value + "\n");

        MLDPChannelProcessorConfig config(cfg);
        EXPECT_EQ(config.alignment(), test_case.expected);
    }
}

TEST(MLDPChannelProcessorConfigTest, ParsesAllTrigger)
{
    struct Case
    {
        const char* yaml_tail;
        TriggerPolicy expected;
        double interval;
    };

    const std::vector<Case> cases{{"trigger: any-update\n", TriggerPolicy::AnyUpdate, 0.0},
                                  {"trigger: all-updated\n", TriggerPolicy::AllUpdated, 0.0},
                                  {"trigger: interval\ntrigger-interval-sec: 0.5\n", TriggerPolicy::Interval, 0.5}};

    for (const auto& test_case : cases)
    {
        const auto cfg = makeConfigFromYaml(std::string(R"yaml(
name: p
sources:
  - s1
)yaml") + test_case.yaml_tail);

        MLDPChannelProcessorConfig config(cfg);
        EXPECT_EQ(config.trigger(), test_case.expected);
        EXPECT_DOUBLE_EQ(config.triggerIntervalSec(), test_case.interval);
    }
}

TEST(MLDPChannelProcessorConfigTest, IntervalRequiresPositiveSec)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
trigger: interval
trigger-interval-sec: 0
)yaml");

    EXPECT_THROW(MLDPChannelProcessorConfig config(cfg), MLDPChannelProcessorConfig::Error);
}

TEST(MLDPChannelProcessorConfigTest, IntervalRequiresPositiveSecAbsent)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
trigger: interval
)yaml");

    EXPECT_THROW(MLDPChannelProcessorConfig config(cfg), MLDPChannelProcessorConfig::Error);
}

TEST(MLDPChannelProcessorConfigTest, EmptyNameAllowed)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: ""
sources:
  - s1
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_TRUE(config.name().empty());
}

TEST(MLDPChannelProcessorConfigTest, MissingNameAllowed)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
sources:
  - s1
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_TRUE(config.name().empty());
}

TEST(MLDPChannelProcessorConfigTest, EmptySourcesReturnsEmpty)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources: []
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_TRUE(config.sources().empty());
    EXPECT_FALSE(config.hasExplicitSources());
}

TEST(MLDPChannelProcessorConfigTest, MissingSourcesReturnsEmpty)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_TRUE(config.sources().empty());
    EXPECT_FALSE(config.hasExplicitSources());
}

TEST(MLDPChannelProcessorConfigTest, SetNameAndSources)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: ""
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    config.setName("injected-name");
    config.setSources({"src:a", "src:b"});

    EXPECT_EQ(config.name(), "injected-name");
    ASSERT_EQ(config.sources().size(), 2u);
    EXPECT_EQ(config.sources()[0], "src:a");
    EXPECT_TRUE(config.hasExplicitSources());
}

TEST(MLDPChannelProcessorConfigTest, UnknownAlignmentThrows)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
alignment: bogus
)yaml");

    EXPECT_THROW(MLDPChannelProcessorConfig config(cfg), MLDPChannelProcessorConfig::Error);
}

TEST(MLDPChannelProcessorConfigTest, UnknownTriggerThrows)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
trigger: bogus
)yaml");

    EXPECT_THROW(MLDPChannelProcessorConfig config(cfg), MLDPChannelProcessorConfig::Error);
}

TEST(MLDPChannelProcessorConfigTest, OutputSourceKeyIgnored)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
output-source: result:pv
output-sources:
  - result:pv
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_EQ(config.name(), "p");
    ASSERT_EQ(config.sources().size(), 1u);
    EXPECT_EQ(config.sources()[0], "s1");
}

TEST(MLDPChannelProcessorConfigTest, ParsesMaxBufferDepth)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
max-buffer-depth: 42
)yaml");

    MLDPChannelProcessorConfig config(cfg);
    EXPECT_EQ(config.maxBufferDepth(), 42u);
}

TEST(MLDPChannelProcessorConfigTest, NegativeMaxBufferDepthThrows)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: p
sources:
  - s1
max-buffer-depth: -1
)yaml");

    EXPECT_THROW(MLDPChannelProcessorConfig config(cfg), MLDPChannelProcessorConfig::Error);
}
