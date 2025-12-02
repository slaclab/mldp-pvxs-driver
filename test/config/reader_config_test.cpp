#include <gtest/gtest.h>

#include "test_reader_config_helpers.h"

namespace mldp_pvxs_driver::config {


TEST(ReaderConfigTest, ParsesValidYaml)
{
    const std::string yaml = R"(
reader:
  type: epics
  options:
    pv_names: ["pv1", "pv2", "pv3"]
)";

    const auto   cfg = makeConfigFromYaml(yaml);
    ReaderConfig readerCfg(cfg);

    EXPECT_TRUE(readerCfg.valid());
    EXPECT_EQ("epics", readerCfg.type());
    const std::vector<std::string> expected{"pv1", "pv2", "pv3"};
    EXPECT_EQ(expected, readerCfg.pvNames());
}

TEST(ReaderConfigTest, ThrowsForMissingSequence)
{
    const std::string yaml = R"(
reader:
  type: epics
  options:
    pv_names: invalid
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(ReaderConfig(cfg)), ReaderConfig::Error);
}

} // namespace mldp_pvxs_driver::config
