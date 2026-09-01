#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <metrics/MetricsSnapshot.h>

#include "../common/MldpMetricsTestUtils.h"
#include "../common/MldpQueryTestUtils.h"
#include "../config/test_config_helpers.h"
#include "../mock/MockArchiverPbHttpServer.h"

#include <chrono>
#include <cctype>
#include <memory>
#include <string>
#include <thread>

using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::testutil;

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;

class MLDPPVXSControllerEpicsArchiverPeriodicTailIntegrationTest : public ::testing::Test
{
protected:
    static std::string sanitizeId(std::string value)
    {
        for (char& ch : value)
        {
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
            {
                ch = '_';
            }
        }
        return value;
    }

    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        ASSERT_NE(info, nullptr);

        const std::string test_id = sanitizeId(std::string(info->test_suite_name()) + "_" + info->name());
        provider_name_ = "test_provider_archiver_tail_" + test_id;
        pv_name_ = "TEST:PV:DOUBLE:" + test_id;

        MockArchiverPbHttpServer::GenerationConfig gen_cfg;
        gen_cfg.min_events_per_second = 4;
        gen_cfg.max_events_per_second = 4;
        gen_cfg.random_seed = 12345;
        server_ = std::make_unique<MockArchiverPbHttpServer>(gen_cfg);
        server_->start();
        ASSERT_GT(server_->port(), 0);

        const std::string yaml = std::string("writer:\n"
                                             "  mldp:\n"
                                             "    - name: mldp_main\n"
                                             "      mldp-pool:\n"
                                             "        provider-name: ")
                                 + provider_name_
                                 + "\n"
                                   "        ingestion-url: dp-ingestion:50051\n"
                                   "        query-url: dp-query:50052\n"
                                   "        min-conn: 1\n"
                                   "        max-conn: 1\n"
                                   "reader:\n"
                                   "  epics-archiver:\n"
                                   "    - name: archiver_tail_reader_test\n"
                                   "      hostname: \""
                                 + server_->baseUrl()
                                 + "\"\n"
                                   "      mode: \"periodic_tail\"\n"
                                   "      poll-interval-sec: 1\n"
                                   "      pvs:\n"
                                   "        - name: \""
                                 + pv_name_ + "\"\n";

        const auto config = makeConfigFromYaml(yaml);
        ASSERT_TRUE(config.valid());
        controller_ = MLDPPVXSController::create(config);
        ASSERT_TRUE(controller_);
        controller_->start();
    }

    void TearDown() override
    {
        if (controller_)
        {
            controller_->stop();
            controller_.reset();
        }
        if (server_)
        {
            server_->stop();
            server_.reset();
        }
    }

    std::unique_ptr<MockArchiverPbHttpServer> server_;
    std::shared_ptr<MLDPPVXSController>       controller_;
    std::string                               provider_name_;
    std::string                               pv_name_;
};

// Verifies controller + epics-archiver(periodic_tail) ingests data into MLDP and uses contiguous periodic windows.
// Also verifies that reader metrics are recorded correctly for the ingested data.
TEST_F(MLDPPVXSControllerEpicsArchiverPeriodicTailIntegrationTest, IngestsPeriodicTailArchiverDataIntoMLDP)
{
    ASSERT_TRUE(server_->waitForRequestCount(3u, std::chrono::seconds(8)));

    const auto history = server_->requestHistory();
    ASSERT_GE(history.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(history[i].pv.has_value());
        ASSERT_TRUE(history[i].from.has_value());
        ASSERT_TRUE(history[i].to.has_value());
        EXPECT_EQ(*history[i].pv, pv_name_);
    }

    // Since lookback defaults to poll interval, the reader should stitch windows contiguously.
    EXPECT_EQ(*history[1].from, *history[0].to);
    EXPECT_EQ(*history[2].from, *history[1].to);

    const auto result = queryAndCollectColumns(
        {pv_name_},
        std::chrono::seconds(10),
        std::chrono::seconds(120));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->contains(pv_name_));
    const auto cols = flattenColumns(result->at(pv_name_));

    // We don't assert an exact count because periodic scheduling jitter and backend dedup behavior can vary.
    ASSERT_GT(cols.size(), 0u);
    ASSERT_TRUE(std::holds_alternative<std::vector<double>>(cols[0].values));

    // Count total samples across all buckets for metric comparison.
    const std::size_t total_samples = std::accumulate(
        cols.begin(), cols.end(), std::size_t{0},
        [](std::size_t acc, const ColumnResult& c)
        {
            if (std::holds_alternative<std::vector<double>>(c.values))
                return acc + std::get<std::vector<double>>(c.values).size();
            return acc;
        });

    // Verify that reader metrics were recorded correctly during data ingestion
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto metrics_text = serializeMetricsText(controller_->metrics());

    // Verify reader events received metric matches ingested data
    const double events_received = getMetricValueForSource(metrics_text,
                                                           "mldp_pvxs_driver_reader_events_received_total",
                                                           pv_name_);
    EXPECT_GT(events_received, 0.0) << "Reader should have recorded events received from archiver";
    // Events received should be at least as many as the data values we got
    EXPECT_GE(events_received, static_cast<double>(total_samples))
        << "Metrics events_received should match or exceed ingested data values";

    // Verify reader events published metric
    const double events_published = getMetricValueForSource(metrics_text,
                                                            "mldp_pvxs_driver_reader_events_total",
                                                            pv_name_);
    EXPECT_GT(events_published, 0.0) << "Reader should have recorded events published to MLDP";
    // Published events should match the number of batches created
    EXPECT_GE(events_published, 1.0) << "Should have at least one published batch";

    // Verify processing time histogram
    const double processing_time_sum = getMetricValueForSource(metrics_text,
                                                               "mldp_pvxs_driver_reader_processing_time_ms_sum",
                                                               pv_name_);
    EXPECT_GT(processing_time_sum, 0.0) << "Reader should have recorded batch processing time";

    const double processing_time_count = getMetricValueForSource(metrics_text,
                                                                 "mldp_pvxs_driver_reader_processing_time_ms_count",
                                                                 pv_name_);
    EXPECT_GT(processing_time_count, 0.0) << "Reader should have at least one processing time observation";
    EXPECT_GE(processing_time_count, 1.0) << "Should have observed processing time for published batches";

    // Verify batching: events received should be >= events published (multiple samples per batch)
    EXPECT_GE(events_received, events_published)
        << "Should have more samples than batches due to time-based batching";
}

// Verifies that EpicsArchiverReader records metrics correctly during periodic tail operation.
TEST_F(MLDPPVXSControllerEpicsArchiverPeriodicTailIntegrationTest, RecordsReaderMetrics)
{
    // Wait for at least 3 archiver requests to complete (multiple poll iterations)
    ASSERT_TRUE(server_->waitForRequestCount(3u, std::chrono::seconds(8)));

    // Give a brief moment for metrics to be flushed
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Collect metrics from the controller
    const mldp_pvxs_driver::metrics::MetricsSnapshot snapshotter;
    const auto                                       metrics_snapshot = snapshotter.getSnapshot(controller_->metrics());

    // Serialize metrics to text for detailed inspection
    const auto metrics_text = serializeMetricsText(controller_->metrics());

    // Verify reader events received metric (samples from archiver)
    const double events_received = getMetricValueForSource(metrics_text,
                                                           "mldp_pvxs_driver_reader_events_received_total",
                                                           pv_name_);
    EXPECT_GT(events_received, 0.0) << "Reader should have received samples from archiver";

    // Verify reader events published metric (batches to MLDP)
    const double events_published = getMetricValueForSource(metrics_text,
                                                            "mldp_pvxs_driver_reader_events_total",
                                                            pv_name_);
    EXPECT_GT(events_published, 0.0) << "Reader should have published event batches";

    // Verify processing time histogram has observations
    const double processing_time_sum = getMetricValueForSource(metrics_text,
                                                               "mldp_pvxs_driver_reader_processing_time_ms_sum",
                                                               pv_name_);
    EXPECT_GT(processing_time_sum, 0.0) << "Reader should have recorded processing time";

    const double processing_time_count = getMetricValueForSource(metrics_text,
                                                                 "mldp_pvxs_driver_reader_processing_time_ms_count",
                                                                 pv_name_);
    EXPECT_GT(processing_time_count, 0.0) << "Reader should have at least one processing time observation";

    EXPECT_GE(events_received, events_published) << "Should have more samples than batches due to batching";
}
