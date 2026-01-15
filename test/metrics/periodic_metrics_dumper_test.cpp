#include <gtest/gtest.h>

#include <PeriodicMetricsDumper.h>

#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>

#include <chrono>
#include <string>

using mldp_pvxs_driver::metrics::Metrics;
using mldp_pvxs_driver::metrics::MetricsConfig;

class PeriodicMetricsDumperTest : public ::testing::Test
{
};

TEST_F(PeriodicMetricsDumperTest, EscapesQuotesInMetricLabels)
{
    MetricsConfig config;
    Metrics       metrics(config);

    metrics.incrementBusPushes(1.0, {{"reader", "pv\"name"}});

    PeriodicMetricsDumper dumper(metrics, "", std::chrono::milliseconds(1));
    const auto            jsonl = dumper.serializeMetricsJsonl();

    EXPECT_NE(
        jsonl.find("\"mldp_pvxs_driver_bus_push_total{reader=\\\"pv\\\\\\\"name\\\"}\""),
        std::string::npos);
}
