#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>

#include "../common/MldpQueryTestUtils.h"
#include "../config/test_config_helpers.h"
#include "../reader/impl/epics_archiver/MockArchiverPbHttpServer.h"

#include <chrono>
#include <memory>
#include <string>

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
  url: dp-ingestion:50051
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

    std::unique_ptr<MockArchiverPbHttpServer>  server_;
    std::shared_ptr<MLDPPVXSController>        controller_;
};

// Verifies controller + epics-archiver(periodic_tail) ingests data into MLDP and uses contiguous periodic windows.
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
}
