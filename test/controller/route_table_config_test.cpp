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
#include <controller/MLDPPVXSControllerConfig.h>
#include "../config/test_config_helpers.h"

namespace mldp_pvxs_driver::controller {

using mldp_pvxs_driver::config::makeConfigFromYaml;

// Helper: minimal valid YAML with writer + reader (required by controller config)
static const std::string kBaseYaml = R"(
writer:
  mldp:
    - name: mldp_main
      mldp-pool:
        provider-name: test
        ingestion-url: dp-ingestion:50051
        query-url: dp-query:50052
        min-conn: 1
        max-conn: 4
reader:
  - epics-pvxs:
      - name: reader_1
        pvs:
          - name: pv1
)";

// 1. NoRoutingConfig_EmptyEntries — backward compatible
TEST(RouteTableConfigTest, NoRoutingConfig_EmptyEntries) {
    const auto cfg = makeConfigFromYaml(kBaseYaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);
    EXPECT_TRUE(controllerCfg.routeEntries().empty());
}

// 2. ValidRoutingConfig_ParsesCorrectly
TEST(RouteTableConfigTest, ValidRoutingConfig_ParsesCorrectly) {
    const std::string yaml = kBaseYaml + R"(
routing:
  mldp_main:
    from: [reader_1, reader_2]
)";
    const auto cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);
    ASSERT_EQ(1u, controllerCfg.routeEntries().size());
    EXPECT_EQ("mldp_main", controllerCfg.routeEntries()[0].first);
    ASSERT_EQ(2u, controllerCfg.routeEntries()[0].second.size());
    EXPECT_EQ("reader_1", controllerCfg.routeEntries()[0].second[0]);
    EXPECT_EQ("reader_2", controllerCfg.routeEntries()[0].second[1]);
}

// 3. AllKeyword_ParsedAsString
TEST(RouteTableConfigTest, AllKeyword_ParsedAsString) {
    const std::string yaml = kBaseYaml + R"(
routing:
  mldp_main:
    from: [all]
)";
    const auto cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);
    ASSERT_EQ(1u, controllerCfg.routeEntries().size());
    ASSERT_EQ(1u, controllerCfg.routeEntries()[0].second.size());
    EXPECT_EQ("all", controllerCfg.routeEntries()[0].second[0]);
}

// 4. MultipleWriterRoutes_ParsedCorrectly
TEST(RouteTableConfigTest, MultipleWriterRoutes_ParsedCorrectly) {
    const std::string yaml = kBaseYaml + R"(
routing:
  mldp_main:
    from: [reader_1]
  other_writer:
    from: [reader_2, reader_3]
)";
    const auto cfg = makeConfigFromYaml(yaml);
    MLDPPVXSControllerConfig controllerCfg(cfg);
    ASSERT_EQ(2u, controllerCfg.routeEntries().size());
}

} // namespace mldp_pvxs_driver::controller
