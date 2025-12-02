#include <gtest/gtest.h>

#include "test_epics_reader_config_helpers.h"

namespace mldp_pvxs_driver::config {

TEST(EpicsReaderConfigTest, ParsesValidEntry)
{
    const std::string yaml = R"(
name: epics_1
pv_names: ["pv1", "pv2", "pv3"]
)";

    const auto        cfg = makeConfigFromYaml(yaml);
    EpicsReaderConfig epicsCfg(cfg);

    EXPECT_TRUE(epicsCfg.valid());
    EXPECT_EQ("epics_1", epicsCfg.name());
    const std::vector<std::string> expected{"pv1", "pv2", "pv3"};
    EXPECT_EQ(expected, epicsCfg.pvNames());
}

TEST(EpicsReaderConfigTest, ThrowsForInvalidPvNamesSequence)
{
    const std::string yaml = R"(
name: epics_bad
pv_names: invalid
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
}

TEST(EpicsReaderConfigTest, ThrowsWhenNameMissing)
{
    const std::string yaml = R"(
pv_names: ["pv1"]
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(EpicsReaderConfig(cfg)), EpicsReaderConfig::Error);
}

} // namespace mldp_pvxs_driver::config
