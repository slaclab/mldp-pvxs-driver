#include <gtest/gtest.h>

#include <controller/MLDPPVXSControllerConfig.h>

#include "test_config_helpers.h"

namespace mldp_pvxs_driver::controller {

using mldp_pvxs_driver::config::makeConfigFromYaml;

TEST(MLDPPVXSControllerConfigTest, ParsesValidConfig)
{
    const std::string yaml = R"(
mldp_pool:
  url: https://mldp.example:443
  max_conn: 4
reader:
  - epics:
      - name: epics_1
        pvs:
          - name: pv1
            option: chan://one
          - name: pv2
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    EXPECT_EQ("https://mldp.example:443", controllerCfg.pool().url);
    EXPECT_EQ(4, controllerCfg.pool().max_conn);

    ASSERT_EQ(1u, controllerCfg.epicsReaders().size());
    const auto& epicsReader = controllerCfg.epicsReaders().front();
    EXPECT_EQ("epics_1", epicsReader.name());
    ASSERT_EQ(2u, epicsReader.pvs().size());
    EXPECT_EQ("pv1", epicsReader.pvs()[0].name);
    EXPECT_EQ("chan://one", epicsReader.pvs()[0].option);
    EXPECT_EQ("pv2", epicsReader.pvs()[1].name);
}

TEST(MLDPPVXSControllerConfigTest, ParsesMultipleEpicsReaders)
{
    const std::string yaml = R"(
mldp_pool:
  url: https://mldp.example:443
  max_conn: 2
reader:
  - epics:
      - name: epics_1
        pvs:
          - name: pv1
      - name: epics_2
        pvs:
          - name: pv2
)";

    const auto               cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);

    ASSERT_TRUE(controllerCfg.valid());
    ASSERT_EQ(2u, controllerCfg.epicsReaders().size());
    EXPECT_EQ("epics_1", controllerCfg.epicsReaders()[0].name());
    ASSERT_EQ(1u, controllerCfg.epicsReaders()[0].pvs().size());
    EXPECT_EQ("pv1", controllerCfg.epicsReaders()[0].pvs()[0].name);
    EXPECT_EQ("epics_2", controllerCfg.epicsReaders()[1].name());
    ASSERT_EQ(1u, controllerCfg.epicsReaders()[1].pvs().size());
    EXPECT_EQ("pv2", controllerCfg.epicsReaders()[1].pvs()[0].name);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWithoutPoolConfig)
{
    const std::string yaml = R"(
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsWhenReaderIsNotSequence)
{
    const std::string yaml = R"(
mldp_pool:
  url: https://mldp.example
  max_conn: 1
reader: invalid
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

TEST(MLDPPVXSControllerConfigTest, ThrowsForUnsupportedReaderType)
{
    const std::string yaml = R"(
mldp_pool:
  url: https://mldp.example
  max_conn: 1
reader:
  - foo:
      - bar
)";

    const auto cfg = makeConfigFromYaml(yaml);
    EXPECT_THROW(static_cast<void>(MLDPPVXSControllerConfig(cfg)), MLDPPVXSControllerConfig::Error);
}

} // namespace mldp_pvxs_driver::controller
