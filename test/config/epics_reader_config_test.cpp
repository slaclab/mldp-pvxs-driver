#include <gtest/gtest.h>

#include <reader/impl/epics/EpicsReaderConfig.h>

#include "test_config_helpers.h"

namespace mldp_pvxs_driver::reader::impl::epics {

using mldp_pvxs_driver::config::makeConfigFromYaml;

TEST(EpicsReaderConfigTest, ParsesValidEntry)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - name: pv1
    option: chan://one
  - name: pv2
    option: ""
  - name: pv3
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    EXPECT_TRUE(epicsCfg.valid());
    EXPECT_EQ("epics_1", epicsCfg.name());
    const std::vector<std::string> expected{"pv1", "pv2", "pv3"};
    EXPECT_EQ(expected, epicsCfg.pvNames());
    ASSERT_EQ(3u, epicsCfg.pvs().size());
    EXPECT_EQ("pv1", epicsCfg.pvs()[0].name);
    EXPECT_EQ("chan://one", epicsCfg.pvs()[0].option);
    EXPECT_FALSE(epicsCfg.pvs()[0].optionConfig.has_value());
    EXPECT_EQ("pv2", epicsCfg.pvs()[1].name);
    EXPECT_EQ("", epicsCfg.pvs()[1].option);
    EXPECT_FALSE(epicsCfg.pvs()[1].optionConfig.has_value());
    EXPECT_EQ("pv3", epicsCfg.pvs()[2].name);
    EXPECT_EQ("", epicsCfg.pvs()[2].option);
    EXPECT_FALSE(epicsCfg.pvs()[2].optionConfig.has_value());
    EXPECT_EQ(EpicsReaderConfig::Backend::Pvxs, epicsCfg.backend());
    EXPECT_EQ(2u, epicsCfg.monitorPollThreads());
    EXPECT_EQ(5u, epicsCfg.monitorPollIntervalMs());
}

TEST(EpicsReaderConfigTest, ParsesBackendAndPollSettings)
{
    const std::string yaml = R"(
name: epics_backend
backend: epics-base
monitor_poll_threads: 3
monitor_poll_interval_ms: 10
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    EXPECT_EQ(EpicsReaderConfig::Backend::EpicsBase, epicsCfg.backend());
    EXPECT_EQ(3u, epicsCfg.monitorPollThreads());
    EXPECT_EQ(10u, epicsCfg.monitorPollIntervalMs());
}

TEST(EpicsReaderConfigTest, ThrowsForInvalidPvsSequence)
{
    const std::string yaml = R"(
name: epics_bad
pvs: invalid
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
}

TEST(EpicsReaderConfigTest, AllowsOptionSubtree)
{
    const std::string yaml = R"(
name: epics_subtree
pvs:
  - name: pv1
    option:
      clamp: true
      limit: 5
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    ASSERT_EQ(1u, epicsCfg.pvs().size());
    const auto& pv = epicsCfg.pvs().front();
    EXPECT_EQ("pv1", pv.name);
    EXPECT_EQ("", pv.option);
    ASSERT_TRUE(pv.optionConfig.has_value());
    EXPECT_TRUE(pv.optionConfig->raw().has_child("clamp"));
    EXPECT_TRUE(pv.optionConfig->raw().has_child("limit"));
}

TEST(EpicsReaderConfigTest, ParsesNTTableRowTimestampOptionMap)
{
    const std::string yaml = R"(
name: epics_nttable
pvs:
  - name: BSAS:TABLE
    option:
      type: nttable-rowts
      tsSeconds: secondsPastEpoch
      tsNanos: nanoseconds
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    ASSERT_TRUE(epicsCfg.valid());
    ASSERT_EQ(1u, epicsCfg.pvs().size());

    const auto& pv = epicsCfg.pvs().front();
    EXPECT_EQ("BSAS:TABLE", pv.name);
    EXPECT_EQ("", pv.option);
    ASSERT_TRUE(pv.optionConfig.has_value());
    EXPECT_EQ("nttable-rowts", pv.optionConfig->get("type"));

    ASSERT_TRUE(pv.nttableRowTs.has_value());
    EXPECT_EQ("secondsPastEpoch", pv.nttableRowTs->tsSecondsField);
    EXPECT_EQ("nanoseconds", pv.nttableRowTs->tsNanosField);
}

TEST(EpicsReaderConfigTest, ThrowsWhenNTTableRowTimestampOptionContainsSourceName)
{
    const std::string yaml = R"(
name: epics_nttable
pvs:
  - name: BSAS:TABLE
    option:
      type: nttable-rowts
      sourceName:
        mode: prefixed
        prefix: "bsas:"
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
}

TEST(EpicsReaderConfigTest, AllowsEmptyPvsSequence)
{
    const std::string yaml = R"(
name: epics_empty
pvs: []
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    EXPECT_TRUE(epicsCfg.valid());
    EXPECT_EQ("epics_empty", epicsCfg.name());
    EXPECT_TRUE(epicsCfg.pvs().empty());
    EXPECT_TRUE(epicsCfg.pvNames().empty());
}

TEST(EpicsReaderConfigTest, ThrowsWhenNameMissing)
{
    const std::string yaml = R"(
pvs:
  - name: pv1
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
}

TEST(EpicsReaderConfigTest, ThrowsWhenPvEntryMissingName)
{
    const std::string yaml = R"(
name: epics_1
pvs:
  - option: foo
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
}

} // namespace mldp_pvxs_driver::reader::impl::epics
