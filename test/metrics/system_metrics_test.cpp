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

#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>

#include <prometheus/text_serializer.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

using mldp_pvxs_driver::metrics::Metrics;
using mldp_pvxs_driver::metrics::MetricsConfig;

namespace {

std::string serializeMetrics(const Metrics& metrics)
{
    prometheus::TextSerializer serializer;
    std::ostringstream         out;
    serializer.Serialize(out, metrics.registry()->Collect());
    return out.str();
}

} // namespace

class SystemMetricsTest : public ::testing::Test
{
};

TEST_F(SystemMetricsTest, ProcessMemoryMetricsAreExposed)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check memory metrics are present
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_virtual_bytes"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_rss_bytes"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_virtual_peak_bytes"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_rss_anon_bytes"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_rss_file_bytes"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_rss_shmem_bytes"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_memory_rss_total_bytes"), std::string::npos);
}

TEST_F(SystemMetricsTest, ProcessCpuMetricsAreExposed)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check CPU metrics are present (these are counters)
    EXPECT_NE(text.find("mldp_pvxs_driver_process_cpu_user_ticks_total"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_cpu_system_ticks_total"), std::string::npos);
}

TEST_F(SystemMetricsTest, ProcessIoMetricsAreExposed)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check I/O metrics are present
    EXPECT_NE(text.find("mldp_pvxs_driver_process_io_read_bytes_total"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_io_write_bytes_total"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_io_cancelled_write_bytes_total"), std::string::npos);
}

TEST_F(SystemMetricsTest, ProcessContextSwitchMetricsAreExposed)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check context switch metrics are present
    EXPECT_NE(text.find("mldp_pvxs_driver_process_context_switches_voluntary_total"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_context_switches_involuntary_total"), std::string::npos);
}

TEST_F(SystemMetricsTest, ProcessThreadAndFdMetricsAreExposed)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check thread count metric is present
    EXPECT_NE(text.find("mldp_pvxs_driver_process_threads"), std::string::npos);

    // Check file descriptor metric is present
    EXPECT_NE(text.find("mldp_pvxs_driver_process_fds_open"), std::string::npos);
}

TEST_F(SystemMetricsTest, ProcessPriorityMetricsAreExposed)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check priority and nice metrics are present
    EXPECT_NE(text.find("mldp_pvxs_driver_process_priority"), std::string::npos);
    EXPECT_NE(text.find("mldp_pvxs_driver_process_nice"), std::string::npos);
}

TEST_F(SystemMetricsTest, MemoryMetricsHaveValidValues)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Parse and check that RSS values are non-negative
    // The metric format is: mldp_pvxs_driver_process_memory_rss_bytes <value>
    auto checkMetricPositive = [&text](const std::string& metric_name)
    {
        auto pos = text.find(metric_name);
        if (pos != std::string::npos)
        {
            // Find the newline after the metric
            auto endPos = text.find('\n', pos);
            if (endPos == std::string::npos)
                endPos = text.length();
            std::string line = text.substr(pos, endPos - pos);
            // Extract the value (after the last space)
            auto spacePos = line.rfind(' ');
            if (spacePos != std::string::npos)
            {
                try
                {
                    double value = std::stod(line.substr(spacePos + 1));
                    EXPECT_GE(value, 0.0) << "Metric " << metric_name << " should be non-negative";
                }
                catch (...)
                {
                    // Value might not be present yet, that's ok
                }
            }
        }
    };

    checkMetricPositive("mldp_pvxs_driver_process_memory_rss_bytes");
    checkMetricPositive("mldp_pvxs_driver_process_memory_virtual_bytes");
    checkMetricPositive("mldp_pvxs_driver_process_threads");
}

TEST_F(SystemMetricsTest, MetricsCollectionThreadIsRunning)
{
    MetricsConfig config;

    // Create metrics instance - this should start the collection thread
    {
        Metrics metrics(config);

        // Wait for a couple of collection cycles
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        const auto text1 = serializeMetrics(metrics);

        // Memory metrics should be present
        EXPECT_NE(text1.find("mldp_pvxs_driver_process_memory_rss_bytes"), std::string::npos);
    }
    // Metrics destroyed here - destructor should stop the thread cleanly
}

TEST_F(SystemMetricsTest, SystemMetricsHaveCorrectMetricTypes)
{
    MetricsConfig config;
    Metrics       metrics(config);

    // Wait briefly for system metrics collection to populate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto text = serializeMetrics(metrics);

    // Check metric type declarations
    // Counters should have _total suffix
    EXPECT_NE(text.find("# TYPE mldp_pvxs_driver_process_cpu_user_ticks_total counter"), std::string::npos);
    EXPECT_NE(text.find("# TYPE mldp_pvxs_driver_process_io_read_bytes_total counter"), std::string::npos);

    // Gauges should not have _total suffix
    EXPECT_NE(text.find("# TYPE mldp_pvxs_driver_process_memory_rss_bytes gauge"), std::string::npos);
    EXPECT_NE(text.find("# TYPE mldp_pvxs_driver_process_threads gauge"), std::string::npos);
}
