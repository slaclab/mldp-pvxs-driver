#include <gtest/gtest.h>

#include <cli/ConfigPrinter.h>

#include "../config/test_config_helpers.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;

TEST(ConfigPrinterTest, FormatsCompactSummaryAcrossReaderTypes)
{
    const std::string yaml = R"(
controller-thread-pool: 2
controller-stream-max-bytes: 1048576
controller-stream-max-age-ms: 250
mldp-pool:
  provider-name: pvxs_provider
  ingestion-url: https://mldp-ingestion.example:443
  query-url: https://mldp-query.example:443
  min-conn: 1
  max-conn: 4
reader:
  - epics-pvxs:
      - name: epics_live
        pvs:
          - name: SYS:PV:ONE
          - name: SYS:PV:TWO
          - name: SYS:PV:THREE
          - name: SYS:PV:FOUR
  - epics-archiver:
      - name: arch_tail
        hostname: https://archiver.example:17668
        mode: periodic_tail
        poll-interval-sec: 5
        pvs:
          - name: ARCH:PV:ONE
metrics:
  endpoint: 0.0.0.0:9464
  scan-interval-seconds: 3
)";

    const auto cfg = makeConfigFromYaml(yaml);
    ASSERT_TRUE(cfg.valid());

    const std::string printed =
        mldp_pvxs_driver::cli::formatStartupConfig(cfg, "/tmp/test-config.yaml");

    EXPECT_NE(printed.find("Effective Startup Configuration"), std::string::npos);
    EXPECT_NE(printed.find("file: /tmp/test-config.yaml"), std::string::npos);
    EXPECT_NE(printed.find("mldp-pool: provider=pvxs_provider"), std::string::npos);
    EXPECT_NE(printed.find("readers: count=2"), std::string::npos);
    EXPECT_NE(printed.find("type=epics-pvxs name=epics_live"), std::string::npos);
    EXPECT_NE(printed.find("pv_preview=[SYS:PV:ONE, SYS:PV:TWO, SYS:PV:THREE, +1 more]"), std::string::npos);
    EXPECT_NE(printed.find("type=epics-archiver name=arch_tail mode=periodic_tail"), std::string::npos);
    EXPECT_NE(printed.find("metrics: enabled endpoint=0.0.0.0:9464 scan_interval_s=3"), std::string::npos);
}

TEST(ConfigPrinterTest, ShowsMetricsDisabledWhenNotConfigured)
{
    const std::string yaml = R"(
controller-thread-pool: 1
mldp-pool:
  provider-name: test_provider
  ingestion-url: dp-ingestion:50051
  query-url: dp-query:50052
  min-conn: 1
  max-conn: 1
reader: []
)";

    const auto cfg = makeConfigFromYaml(yaml);
    ASSERT_TRUE(cfg.valid());

    const std::string printed =
        mldp_pvxs_driver::cli::formatStartupConfig(cfg, "config.yaml");

    EXPECT_NE(printed.find("metrics: disabled"), std::string::npos);
    EXPECT_NE(printed.find("readers: count=0"), std::string::npos);
}
