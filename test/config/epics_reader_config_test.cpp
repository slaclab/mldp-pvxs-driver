#include <gtest/gtest.h>

#include <reader/impl/epics/base/EpicsBaseReaderConfig.h>
#include <reader/impl/epics/pvxs/EpicsPVXSReaderConfig.h>
#include <reader/impl/epics/shared/EpicsReaderConfig.h>

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
}

TEST(EpicsReaderConfigTest, RejectsBackendField)
{
    const std::string yaml = R"(
name: epics_backend
backend: epics-base
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
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
      type: slac-bsas-table
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
    EXPECT_EQ("slac-bsas-table", pv.optionConfig->get("type"));

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
      type: slac-bsas-table
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

TEST(EpicsBaseReaderConfigTest, ParsesMonitorPollFields)
{
    const std::string yaml = R"(
name: base_reader
monitor-poll-threads: 4
monitor-poll-interval-ms: 10
pvs:
  - name: pv1
)";

    const auto              cfg = makeConfigFromYaml(yaml);
    EpicsBaseReaderConfig   baseCfg(cfg);

    EXPECT_TRUE(baseCfg.valid());
    EXPECT_EQ("base_reader", baseCfg.name());
    EXPECT_EQ(4u, baseCfg.monitorPollThreads());
    EXPECT_EQ(10u, baseCfg.monitorPollIntervalMs());
}

TEST(EpicsBaseReaderConfigTest, DefaultsMonitorPollFields)
{
    const std::string yaml = R"(
name: base_defaults
pvs:
  - name: pv1
)";

    const auto            cfg = makeConfigFromYaml(yaml);
    EpicsBaseReaderConfig baseCfg(cfg);

    EXPECT_EQ(2u, baseCfg.monitorPollThreads());
    EXPECT_EQ(5u, baseCfg.monitorPollIntervalMs());
}

TEST(EpicsPVXSReaderConfigTest, ConstructsFromYaml)
{
    const std::string yaml = R"(
name: pvxs_reader
thread-pool: 8
pvs:
  - name: pv1
)";

    const auto             cfg = makeConfigFromYaml(yaml);
    EpicsPVXSReaderConfig  pvxsCfg(cfg);

    EXPECT_TRUE(pvxsCfg.valid());
    EXPECT_EQ("pvxs_reader", pvxsCfg.name());
    EXPECT_EQ(8u, pvxsCfg.threadPoolSize());
}

TEST(EpicsReaderConfigTest, ParsesColumnBatchSizeInSlacBsasTableOption)
{
    const std::string yaml = R"(
name: epics_bsas_batch
pvs:
  - name: BSAS:TABLE
    option:
      type: slac-bsas-table
      tsSeconds: secondsPastEpoch
      tsNanos: nanoseconds
      column-batch-size: 10
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    ASSERT_TRUE(epicsCfg.valid());
    ASSERT_EQ(1u, epicsCfg.pvs().size());

    const auto& pv = epicsCfg.pvs().front();
    ASSERT_TRUE(pv.nttableRowTs.has_value());
    EXPECT_EQ(10u, pv.nttableRowTs->columnBatchSize);
}

TEST(EpicsReaderConfigTest, ColumnBatchSizeDefaultsToOneWhenNotSpecified)
{
    const std::string yaml = R"(
name: epics_bsas_default
pvs:
  - name: BSAS:TABLE
    option:
      type: slac-bsas-table
      tsSeconds: secondsPastEpoch
      tsNanos: nanoseconds
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    ASSERT_TRUE(epicsCfg.valid());
    const auto& pv = epicsCfg.pvs().front();
    ASSERT_TRUE(pv.nttableRowTs.has_value());
    EXPECT_EQ(1u, pv.nttableRowTs->columnBatchSize);
}

} // namespace mldp_pvxs_driver::reader::impl::epics
