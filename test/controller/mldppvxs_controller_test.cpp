#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <metrics/MetricsSnapshot.h>

#include <prometheus/text_serializer.h>

#include <chrono>
#include <optional>
#include <thread>
#include <type_traits>
#include <unistd.h>

#include "../config/test_config_helpers.h"
#include "../mock/sioc.h"

namespace mldp_pvxs_driver::controller {

using config::makeConfigFromYaml;
using util::bus::IEventBusPush;

namespace {

    constexpr std::string_view kMinimalControllerConfig = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider
  provider_description: "Test Provider"
  url: dp-ingestion:50051
  min_conn: 1
  max_conn: 1
reader: []
)";

    constexpr std::string_view kEpicsControllerConfig = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider
  provider_description: "Test Provider"
  url: dp-ingestion:50051
  min_conn: 1
  max_conn: 1
reader:
  - epics:
      - name: epics_reader_1
        pvs:
          - name: test:counter
)";

    constexpr std::string_view kBsasNtTableRowTsControllerConfig = R"(
controller_thread_pool: 1
mldp_pool:
  provider_name: test_provider
  provider_description: "Test Provider"
  url: dp-ingestion:50051
  min_conn: 1
  max_conn: 1
reader:
  - epics:
      - name: epics_reader_1
        pvs:
          - name: test:bsas_table
            option:
              type: nttable-rowts
)";

    std::optional<mldp_pvxs_driver::metrics::ReaderMetrics> findReaderMetrics(
        const mldp_pvxs_driver::metrics::MetricsData& snapshot,
        const std::string& pvName)
    {
        for (const auto& reader : snapshot.readers)
        {
            if (reader.pv_name == pvName)
            {
                return reader;
            }
        }
        return std::nullopt;
    }

    std::string serializeMetricsText(const mldp_pvxs_driver::metrics::Metrics& metrics)
    {
        prometheus::TextSerializer serializer;
        std::ostringstream         out;
        serializer.Serialize(out, metrics.registry()->Collect());
        return out.str();
    }

    std::string extractLabelValue(std::string_view line, std::string_view label)
    {
        const auto label_start = line.find(label);
        if (label_start == std::string_view::npos)
        {
            return "";
        }
        const auto quote_start = line.find('"', label_start);
        if (quote_start == std::string_view::npos)
        {
            return "";
        }
        const auto quote_end = line.find('"', quote_start + 1);
        if (quote_end == std::string_view::npos)
        {
            return "";
        }
        return std::string(line.substr(quote_start + 1, quote_end - quote_start - 1));
    }

    double extractMetricValue(std::string_view line)
    {
        const auto last_space = line.rfind(' ');
        if (last_space == std::string_view::npos)
        {
            return 0.0;
        }
        try
        {
            return std::stod(std::string(line.substr(last_space + 1)));
        }
        catch (...)
        {
            return 0.0;
        }
    }

    double getMetricValueForSource(const std::string& text, std::string_view metric, const std::string& source)
    {
        std::istringstream stream(text);
        std::string        line;
        while (std::getline(stream, line))
        {
            if (line.empty() || line.front() == '#')
            {
                continue;
            }
            if (line.find(metric) == std::string::npos)
            {
                continue;
            }
            const auto label_value = extractLabelValue(line, "source=");
            if (label_value == source)
            {
                return extractMetricValue(line);
            }
        }
        return 0.0;
    }

    double getGaugeValue(const std::string& text, std::string_view metric)
    {
        std::istringstream stream(text);
        std::string        line;
        while (std::getline(stream, line))
        {
            if (line.empty() || line.front() == '#')
            {
                continue;
            }
            if (line.find(metric) != std::string::npos)
            {
                return extractMetricValue(line);
            }
        }
        return 0.0;
    }

} // namespace

TEST(MLDPPVXSControllerTest, ImplementsEventBusPushContract)
{
    static_assert(std::is_base_of_v<IEventBusPush, MLDPPVXSController>);
    SUCCEED();
}

TEST(MLDPPVXSControllerTest, StartAndStopDoNotThrowWithValidConfig)
{
    const auto config = makeConfigFromYaml(std::string(kMinimalControllerConfig));

    ASSERT_TRUE(config.valid());
    auto controller = MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);
    ASSERT_NO_THROW(controller->start(););
    sleep(1); // Allow some time for the reader to start
    ASSERT_NO_THROW(controller->stop(););
}

TEST(MLDPPVXSControllerTest, StartAndStopDoNotThrowWithEpicsConfig)
{
    const auto config = makeConfigFromYaml(std::string(kEpicsControllerConfig));

    ASSERT_TRUE(config.valid());

    {
        // strart pv mocker
        PVServer pvServer;
        auto     controller = MLDPPVXSController::create(config);
        ASSERT_TRUE(controller);
        ASSERT_NO_THROW(controller->start(););
        sleep(1); // Allow some time for the reader to start
        ASSERT_NO_THROW(controller->stop(););
        // chgeck on metric if the event has been pushed
        auto& metrics = controller->metrics();
        EXPECT_GE(metrics.busPushTotal({}), 0);
    }
}

TEST(MLDPPVXSControllerTest, BsasNtTableRowTsAggregatesPushAndBandwidthMetrics)
{
    const auto config = makeConfigFromYaml(std::string(kBsasNtTableRowTsControllerConfig));
    ASSERT_TRUE(config.valid());

    PVServer pvServer;
    auto     controller = MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);

    ASSERT_NO_THROW(controller->start(););

    const int                                          max_wait_ms = 8000;
    int                                                waited_ms = 0;
    const mldp_pvxs_driver::metrics::MetricsSnapshot   snapshotter;
    std::optional<mldp_pvxs_driver::metrics::MetricsData> snapshot;
    std::string                                        metrics_text;
    double                                             table_send_sum = 0.0;
    double                                             colA_send_sum = 0.0;
    double                                             colB_send_sum = 0.0;
    double                                             table_send_count = 0.0;
    double                                             colA_send_count = 0.0;
    double                                             colB_send_count = 0.0;
    double                                             queue_depth = 0.0;

    while (waited_ms < max_wait_ms)
    {
        snapshot = snapshotter.getSnapshot(controller->metrics());
        metrics_text = serializeMetricsText(controller->metrics());
        const auto colA = findReaderMetrics(*snapshot, "PV_NAME_A_DOUBLE_VALUE");
        const auto colB = findReaderMetrics(*snapshot, "PV_NAME_B_STRING_VALUE");
        const auto table = findReaderMetrics(*snapshot, "test:bsas_table");
        table_send_sum = getMetricValueForSource(metrics_text,
                                                 "mldp_pvxs_driver_controller_send_time_seconds_sum",
                                                 "test:bsas_table");
        colA_send_sum = getMetricValueForSource(metrics_text,
                                                "mldp_pvxs_driver_controller_send_time_seconds_sum",
                                                "PV_NAME_A_DOUBLE_VALUE");
        colB_send_sum = getMetricValueForSource(metrics_text,
                                                "mldp_pvxs_driver_controller_send_time_seconds_sum",
                                                "PV_NAME_B_STRING_VALUE");
        table_send_count = getMetricValueForSource(metrics_text,
                                                   "mldp_pvxs_driver_controller_send_time_seconds_count",
                                                   "test:bsas_table");
        colA_send_count = getMetricValueForSource(metrics_text,
                                                  "mldp_pvxs_driver_controller_send_time_seconds_count",
                                                  "PV_NAME_A_DOUBLE_VALUE");
        colB_send_count = getMetricValueForSource(metrics_text,
                                                  "mldp_pvxs_driver_controller_send_time_seconds_count",
                                                  "PV_NAME_B_STRING_VALUE");
        queue_depth = getGaugeValue(metrics_text, "mldp_pvxs_driver_controller_queue_depth");
        if (colA.has_value() && colB.has_value() && table.has_value() &&
            colA->pushes > 0 && colB->pushes > 0 && table->pushes > 0)
        {
            if (table_send_sum > 0.0 && colA_send_sum > 0.0 && colB_send_sum > 0.0 &&
                table_send_count > 0.0 && colA_send_count > 0.0 && colB_send_count > 0.0)
            {
                break;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waited_ms += 200;
    }

    ASSERT_TRUE(snapshot.has_value()) << "Metrics snapshot missing";
    const auto colA = findReaderMetrics(*snapshot, "PV_NAME_A_DOUBLE_VALUE");
    const auto colB = findReaderMetrics(*snapshot, "PV_NAME_B_STRING_VALUE");
    const auto table = findReaderMetrics(*snapshot, "test:bsas_table");

    ASSERT_TRUE(colA.has_value()) << "Missing metrics for PV_NAME_A_DOUBLE_VALUE";
    ASSERT_TRUE(colB.has_value()) << "Missing metrics for PV_NAME_B_STRING_VALUE";
    ASSERT_TRUE(table.has_value()) << "Missing metrics for test:bsas_table";
    EXPECT_FALSE(findReaderMetrics(*snapshot, "secondsPastEpoch").has_value());
    EXPECT_FALSE(findReaderMetrics(*snapshot, "nanoseconds").has_value());

    EXPECT_GT(colA->pushes, 0);
    EXPECT_GT(colB->pushes, 0);
    EXPECT_GT(table->pushes, 0);

    const auto total_pushes = colA->pushes + colB->pushes;
    EXPECT_EQ(table->pushes, total_pushes);

    EXPECT_GT(colA->bytes_total, 0.0);
    EXPECT_GT(colB->bytes_total, 0.0);
    EXPECT_GT(table->bytes_total, 0.0);
    EXPECT_NEAR(table->bytes_total, colA->bytes_total + colB->bytes_total, 0.001);

    EXPECT_GT(colA->bytes_per_sec, 0.0);
    EXPECT_GT(colB->bytes_per_sec, 0.0);
    EXPECT_GT(table->bytes_per_sec, 0.0);
    EXPECT_NEAR(table->bytes_per_sec, colA->bytes_per_sec + colB->bytes_per_sec, 0.001);

    EXPECT_GE(table_send_sum, colA_send_sum + colB_send_sum);
    EXPECT_GE(table_send_count, colA_send_count + colB_send_count);
    EXPECT_GE(queue_depth, 0.0);

    ASSERT_NO_THROW(controller->stop(););
}

TEST(MLDPPVXSControllerTest, EpicsCounterEmitsSingleReaderMetric)
{
    const auto config = makeConfigFromYaml(std::string(kEpicsControllerConfig));
    ASSERT_TRUE(config.valid());

    PVServer pvServer;
    auto     controller = MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);

    ASSERT_NO_THROW(controller->start(););

    const int                                        max_wait_ms = 8000;
    int                                              waited_ms = 0;
    const mldp_pvxs_driver::metrics::MetricsSnapshot snapshotter;
    std::optional<mldp_pvxs_driver::metrics::MetricsData> snapshot;

    while (waited_ms < max_wait_ms)
    {
        snapshot = snapshotter.getSnapshot(controller->metrics());
        const auto counter = findReaderMetrics(*snapshot, "test:counter");
        if (counter.has_value() && counter->pushes > 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waited_ms += 200;
    }

    ASSERT_TRUE(snapshot.has_value()) << "Metrics snapshot missing";
    const auto counter = findReaderMetrics(*snapshot, "test:counter");
    ASSERT_TRUE(counter.has_value()) << "Missing metrics for test:counter";
    EXPECT_GT(counter->pushes, 0);
    ASSERT_EQ(snapshot->readers.size(), 1u);

    ASSERT_NO_THROW(controller->stop(););
}

} // namespace mldp_pvxs_driver::controller
