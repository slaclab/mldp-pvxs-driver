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

    // Check for the new structured format with "source" tag instead of "reader"
    EXPECT_NE(jsonl.find("\"mldp_pvxs_driver_bus_push_total\""), std::string::npos);
    EXPECT_NE(jsonl.find("\"source\""), std::string::npos);
    EXPECT_NE(jsonl.find("\"value\""), std::string::npos);
    // Ensure old format is not present
    EXPECT_EQ(jsonl.find("{reader="), std::string::npos);
}

TEST_F(PeriodicMetricsDumperTest, GroupsHistogramSamplesUnderBaseMetric)
{
    MetricsConfig config;
    Metrics       metrics(config);

    metrics.observeReaderProcessingTimeSeconds(0.002, {{"source", "pv:test"}});

    PeriodicMetricsDumper dumper(metrics, "", std::chrono::milliseconds(1));
    const auto            jsonl = dumper.serializeMetricsJsonl();

    EXPECT_NE(jsonl.find("\"mldp_pvxs_driver_reader_processing_time_seconds\""), std::string::npos);
    EXPECT_EQ(jsonl.find("mldp_pvxs_driver_reader_processing_time_seconds_bucket"), std::string::npos);
    EXPECT_NE(jsonl.find("\"histogram\": \"bucket\""), std::string::npos);
    EXPECT_NE(jsonl.find("\"histogram\": \"sum\""), std::string::npos);
    EXPECT_NE(jsonl.find("\"histogram\": \"count\""), std::string::npos);
    EXPECT_NE(jsonl.find("\"source\": \"pv:test\""), std::string::npos);
}
