#include <gtest/gtest.h>

#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>

#include <prometheus/text_serializer.h>

#include <sstream>
#include <string>

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

TEST(MetricsHistogramTest, RecordsReaderProcessingTimeWithSourceTag)
{
    MetricsConfig config;
    Metrics       metrics(config);

    metrics.observeReaderProcessingTimeMs(2.0, {{"source", "pv:test"}});

    const auto        text = serializeMetrics(metrics);
    const std::string count_line = "mldp_pvxs_driver_reader_processing_time_ms_count{source=\"pv:test\"} 1";
    const std::string sum_line = "mldp_pvxs_driver_reader_processing_time_ms_sum{source=\"pv:test\"}";
    const std::string bucket_prefix = "mldp_pvxs_driver_reader_processing_time_ms_bucket{source=\"pv:test\"";

    EXPECT_NE(text.find(count_line), std::string::npos);
    EXPECT_NE(text.find(sum_line), std::string::npos);
    EXPECT_NE(text.find(bucket_prefix), std::string::npos);
}
