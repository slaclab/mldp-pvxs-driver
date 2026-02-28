#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <metrics/MetricsSnapshot.h>

#include "../common/MldpMetricsTestUtils.h"
#include "../common/MldpQueryTestUtils.h"
#include "../config/test_config_helpers.h"
#include "../reader/impl/epics_archiver/MockArchiverPbHttpServer.h"

#include <chrono>
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
    void SetUp() override
    {
        MockArchiverPbHttpServer::GenerationConfig gen_cfg;
        gen_cfg.min_events_per_second = 4;
        gen_cfg.max_events_per_second = 4;
        gen_cfg.random_seed = 12345;
        server_ = std::make_unique<MockArchiverPbHttpServer>(gen_cfg);
        server_->start();
        ASSERT_GT(server_->port(), 0);

        const std::string yaml = std::string(R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider_archiver_tail
  provider_description: "Archiver Tail Integration Test Provider"
  ingestion_url: dp-ingestion:50051
  min_conn: 1
  max_conn: 1
reader:
  - epics-archiver:
      - name: archiver_tail_reader_test
        hostname: ")") + server_->baseUrl() +
                                 R"("
        mode: "periodic_tail"
        poll_interval_sec: 1
        batch_duration_sec: 2
        pvs:
          - name: "TEST:PV:DOUBLE"
)";

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
        EXPECT_EQ(*history[i].pv, "TEST:PV:DOUBLE");
    }

    // Since lookback defaults to poll interval, the reader should stitch windows contiguously.
    EXPECT_EQ(*history[1].from, *history[0].to);
    EXPECT_EQ(*history[2].from, *history[1].to);

    const auto result = queryAndCollectColumns(
        {"TEST:PV:DOUBLE"},
        std::chrono::seconds(10),
        std::chrono::seconds(120));
    ASSERT_TRUE(result.has_value());
    const auto& column = result->at("TEST:PV:DOUBLE");

    // We don't assert an exact count because periodic scheduling jitter and backend dedup behavior can vary.
    ASSERT_GT(column.datavalues_size(), 0);
    EXPECT_EQ(column.name(), "TEST:PV:DOUBLE");
    EXPECT_EQ(column.datavalues(0).value_case(), DataValue::kDoubleValue);

    // Verify that reader metrics were recorded correctly during data ingestion
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto metrics_text = serializeMetricsText(controller_->metrics());

    // Verify reader events received metric matches ingested data
    const double events_received = getMetricValueForSource(metrics_text,
                                                           "mldp_pvxs_driver_reader_events_received_total",
                                                           "TEST:PV:DOUBLE");
    EXPECT_GT(events_received, 0.0) << "Reader should have recorded events received from archiver";
    // Events received should be at least as many as the data values we got
    EXPECT_GE(events_received, column.datavalues_size())
        << "Metrics events_received should match or exceed ingested data values";

    // Verify reader events published metric
    const double events_published = getMetricValueForSource(metrics_text,
                                                            "mldp_pvxs_driver_reader_events_total",
                                                            "TEST:PV:DOUBLE");
    EXPECT_GT(events_published, 0.0) << "Reader should have recorded events published to MLDP";
    // Published events should match the number of batches created
    EXPECT_GE(events_published, 1.0) << "Should have at least one published batch";

    // Verify processing time histogram
    const double processing_time_sum = getMetricValueForSource(metrics_text,
                                                               "mldp_pvxs_driver_reader_processing_time_ms_sum",
                                                               "TEST:PV:DOUBLE");
    EXPECT_GT(processing_time_sum, 0.0) << "Reader should have recorded batch processing time";

    const double processing_time_count = getMetricValueForSource(metrics_text,
                                                                 "mldp_pvxs_driver_reader_processing_time_ms_count",
                                                                 "TEST:PV:DOUBLE");
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
                                                           "TEST:PV:DOUBLE");
    EXPECT_GT(events_received, 0.0) << "Reader should have received samples from archiver";

    // Verify reader events published metric (batches to MLDP)
    const double events_published = getMetricValueForSource(metrics_text,
                                                            "mldp_pvxs_driver_reader_events_total",
                                                            "TEST:PV:DOUBLE");
    EXPECT_GT(events_published, 0.0) << "Reader should have published event batches";

    // Verify processing time histogram has observations
    const double processing_time_sum = getMetricValueForSource(metrics_text,
                                                               "mldp_pvxs_driver_reader_processing_time_ms_sum",
                                                               "TEST:PV:DOUBLE");
    EXPECT_GT(processing_time_sum, 0.0) << "Reader should have recorded processing time";

    const double processing_time_count = getMetricValueForSource(metrics_text,
                                                                 "mldp_pvxs_driver_reader_processing_time_ms_count",
                                                                 "TEST:PV:DOUBLE");
    EXPECT_GT(processing_time_count, 0.0) << "Reader should have at least one processing time observation";

    // Verify events published is at least as many as the expected batches
    // (accounting for batch_duration_sec: 2, which may create multiple batches per poll)
    EXPECT_GE(events_received, events_published) << "Should have more samples than batches due to batching";
}
