#include <gtest/gtest.h>

#include <cstdlib>

#include <reader/impl/epics/base/EpicsBaseReaderConfig.h>
#include <reader/impl/epics/pvxs/EpicsPVXSReaderConfig.h>
#include <reader/impl/epics/shared/EpicsReaderConfig.h>
#include <reader/impl/epics/shared/PvxsClientConfig.h>

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

    const auto            cfg = makeConfigFromYaml(yaml);
    EpicsBaseReaderConfig baseCfg(cfg);

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

    const auto            cfg = makeConfigFromYaml(yaml);
    EpicsPVXSReaderConfig pvxsCfg(cfg);

    EXPECT_TRUE(pvxsCfg.valid());
    EXPECT_EQ("pvxs_reader", pvxsCfg.name());
    EXPECT_EQ(8u, pvxsCfg.threadPoolSize());
}

TEST(EpicsPVXSReaderConfigTest, AcceptsReaderLocalPvxsEnvironment)
{
    const auto cfg = makeConfigFromYaml(R"(
name: pvxs_reader
environment:
  EPICS_PVA_ADDR_LIST: "192.0.2.10:5076 192.0.2.11:5076"
  EPICS_PVA_AUTO_ADDR_LIST: "NO"
  EPICS_PVA_INTF_ADDR_LIST: "10.0.0.10 10.0.0.11"
  EPICS_PVA_BROADCAST_PORT: "5077"
  EPICS_PVA_NAME_SERVERS: "192.0.2.30:5076"
  EPICS_PVA_CONN_TMO: "42.5"
pvs:
  - name: pv1
    )");

    EXPECT_NO_THROW(static_cast<void>(EpicsPVXSReaderConfig(cfg)));
}

TEST(EpicsPVXSReaderConfigTest, RejectsInvalidReaderLocalPvxsEnvironment)
{
    const std::vector<std::string> environments{
        "environment: invalid",
        "environment:\n  NOT_EPICS_PVA: value",
        "environment:\n  EPICS_PVA_ADDR_LIST:\n    - 192.0.2.10:5076",
    };

    for (const auto& environment : environments)
    {
        const auto cfg = makeConfigFromYaml("name: pvxs_reader\n" + environment + "\npvs:\n  - name: pv1\n");
        EXPECT_THROW(static_cast<void>(EpicsPVXSReaderConfig(cfg)), EpicsPVXSReaderConfig::Error) << environment;
    }
}

TEST(EpicsPVXSReaderConfigTest, AcceptsPvxsOwnedDefinitionValues)
{
    const auto cfg = makeConfigFromYaml(R"(
name: pvxs_reader
environment:
  EPICS_PVA_UNSUPPORTED: value
  EPICS_PVA_AUTO_ADDR_LIST: maybe
pvs:
  - name: pv1
)");

    EXPECT_NO_THROW(static_cast<void>(EpicsPVXSReaderConfig(cfg)));
    EXPECT_NO_THROW(static_cast<void>(PvxsClientConfig::buildConfig(cfg, "epics-pvxs")));
}

TEST(PvxsClientConfigTest, KeepsReaderOverridesIndependentAndDoesNotModifyProcessEnvironment)
{
    const auto first = makeConfigFromYaml(R"(
environment:
  EPICS_PVA_ADDR_LIST: "192.0.2.10:5076"
  EPICS_PVA_AUTO_ADDR_LIST: "NO"
)");
    const auto second = makeConfigFromYaml(R"(
environment:
  EPICS_PVA_ADDR_LIST: "192.0.2.20:5076"
  EPICS_PVA_AUTO_ADDR_LIST: "YES"
)");

    const char* const processValue = std::getenv("EPICS_PVA_ADDR_LIST");
    const std::string before = processValue ? processValue : "";
    const bool        hadProcessValue = processValue != nullptr;

    const auto firstConfig = PvxsClientConfig::buildConfig(first, "epics-pvxs");
    const auto secondConfig = PvxsClientConfig::buildConfig(second, "epics-ds-metadata");

    ASSERT_EQ(1u, firstConfig.addressList.size());
    ASSERT_EQ(1u, secondConfig.addressList.size());
    EXPECT_NE(firstConfig.addressList.front(), secondConfig.addressList.front());
    EXPECT_FALSE(firstConfig.autoAddrList);
    EXPECT_TRUE(secondConfig.autoAddrList);

    const char* const processValueAfter = std::getenv("EPICS_PVA_ADDR_LIST");
    EXPECT_EQ(hadProcessValue, processValueAfter != nullptr);
    if (hadProcessValue)
        EXPECT_EQ(before, processValueAfter);
}

TEST(PvxsClientConfigTest, WithoutOverridesUsesInheritedPvxsDefaults)
{
    const auto inherited = pvxs::client::Config::fromEnv();
    const auto readerConfig = PvxsClientConfig::buildConfig(makeConfigFromYaml("name: pvxs_reader\npvs:\n  - name: pv1\n"), "epics-pvxs");

    EXPECT_EQ(inherited.addressList, readerConfig.addressList);
    EXPECT_EQ(inherited.autoAddrList, readerConfig.autoAddrList);
    EXPECT_EQ(inherited.interfaces, readerConfig.interfaces);
    EXPECT_EQ(inherited.udp_port, readerConfig.udp_port);
    EXPECT_EQ(inherited.nameServers, readerConfig.nameServers);
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
