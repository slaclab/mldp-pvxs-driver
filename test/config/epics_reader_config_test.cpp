#include <gtest/gtest.h>

#include "test_epics_reader_config_helpers.h"

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
    EXPECT_EQ("pv2", epicsCfg.pvs()[1].name);
    EXPECT_EQ("", epicsCfg.pvs()[1].option);
    EXPECT_EQ("pv3", epicsCfg.pvs()[2].name);
    EXPECT_EQ("", epicsCfg.pvs()[2].option);
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
