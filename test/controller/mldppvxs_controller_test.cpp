#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <metrics/MetricsSnapshot.h>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>
#include <ingestion.grpc.pb.h>

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
  - epics-pvxs:
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
  - epics-pvxs:
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

    mldp_pvxs_driver::metrics::ReaderMetrics aggregateReaderMetrics(
        const mldp_pvxs_driver::metrics::MetricsData& snapshot)
    {
        mldp_pvxs_driver::metrics::ReaderMetrics aggregated;
        for (const auto& reader : snapshot.readers)
        {
            aggregated.pushes += reader.pushes;
            aggregated.bytes_total += reader.bytes_total;
            aggregated.bytes_per_sec += reader.bytes_per_sec;
        }
        return aggregated;
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

    class TestIngestionService final : public dp::service::ingestion::DpIngestionService::Service
    {
    public:
        std::atomic<int> stream_count{0};
        std::atomic<int> request_count{0};
        std::atomic<int> stream_close_count{0};

        grpc::Status registerProvider(grpc::ServerContext*,
                                      const dp::service::ingestion::RegisterProviderRequest* request,
                                      dp::service::ingestion::RegisterProviderResponse* response) override
        {
            auto* result = response->mutable_registrationresult();
            result->set_providerid("test-provider-id");
            result->set_providername(request->providername());
            result->set_isnewprovider(true);
            return grpc::Status::OK;
        }

        grpc::Status ingestDataStream(grpc::ServerContext*,
                                      grpc::ServerReader<dp::service::ingestion::IngestDataRequest>* reader,
                                      dp::service::ingestion::IngestDataStreamResponse*) override
        {
            stream_count.fetch_add(1, std::memory_order_relaxed);
            dp::service::ingestion::IngestDataRequest request;
            while (reader->Read(&request))
            {
                request_count.fetch_add(1, std::memory_order_relaxed);
            }
            stream_close_count.fetch_add(1, std::memory_order_relaxed);
            return grpc::Status::OK;
        }
    };

    bool waitForCount(std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (counter.load(std::memory_order_relaxed) >= target)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return counter.load(std::memory_order_relaxed) >= target;
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
    double                                             table_send_count = 0.0;
    double                                             queue_depth = 0.0;

    while (waited_ms < max_wait_ms)
    {
        snapshot = snapshotter.getSnapshot(controller->metrics());
        metrics_text = serializeMetricsText(controller->metrics());
        const auto aggregated = aggregateReaderMetrics(*snapshot);
        table_send_sum = getMetricValueForSource(metrics_text,
                                                 "mldp_pvxs_driver_controller_send_time_seconds_sum",
                                                 "test:bsas_table");
        table_send_count = getMetricValueForSource(metrics_text,
                                                   "mldp_pvxs_driver_controller_send_time_seconds_count",
                                                   "test:bsas_table");
        queue_depth = getGaugeValue(metrics_text, "mldp_pvxs_driver_controller_queue_depth");
        if (!snapshot->readers.empty())
        {
            const bool has_pushes = aggregated.pushes > 0;
            const bool has_bytes = aggregated.bytes_total > 0.0;
            const bool has_rates = aggregated.bytes_per_sec > 0.0;
            const bool has_send_metrics = table_send_sum > 0.0 && table_send_count > 0.0;
            if (has_pushes && has_bytes && has_rates && has_send_metrics)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waited_ms += 200;
    }

    ASSERT_TRUE(snapshot.has_value()) << "Metrics snapshot missing";
    ASSERT_FALSE(snapshot->readers.empty()) << "Missing reader metrics";
    const auto aggregated = aggregateReaderMetrics(*snapshot);

    EXPECT_GT(aggregated.pushes, 0);
    EXPECT_GT(aggregated.bytes_total, 0.0);
    EXPECT_GT(aggregated.bytes_per_sec, 0.0);
    EXPECT_GT(table_send_sum, 0.0);
    EXPECT_GT(table_send_count, 0.0);
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
        const auto aggregated = aggregateReaderMetrics(*snapshot);
        if ((counter.has_value() && counter->pushes > 0) ||
            (!snapshot->readers.empty() && aggregated.pushes > 0))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waited_ms += 200;
    }

    ASSERT_TRUE(snapshot.has_value()) << "Metrics snapshot missing";
    const auto counter = findReaderMetrics(*snapshot, "test:counter");
    ASSERT_FALSE(snapshot->readers.empty()) << "Missing reader metrics";
    if (counter.has_value())
    {
        EXPECT_GT(counter->pushes, 0);
    }
    else
    {
        const auto aggregated = aggregateReaderMetrics(*snapshot);
        EXPECT_GT(aggregated.pushes, 0);
    }

    ASSERT_NO_THROW(controller->stop(););
}

TEST(MLDPPVXSControllerTest, IdleStreamRotationStartsNewStreamAfterMaxAge)
{
    TestIngestionService service;
    grpc::ServerBuilder  builder;
    int                  port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    std::ostringstream yaml;
    yaml << "controller_thread_pool: 1\n"
         << "controller_stream_max_age_ms: 150\n"
         << "mldp_pool:\n"
         << "  provider_name: test_provider\n"
         << "  provider_description: \"Test Provider\"\n"
         << "  url: 127.0.0.1:" << port << "\n"
         << "  min_conn: 1\n"
         << "  max_conn: 1\n"
         << "reader: []\n";

    const auto config = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(config.valid());

    auto controller = MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);
    controller->start();

    IEventBusPush::EventBatch batch;
    batch.root_source = "test-root";
    batch.tags = {"test"};
    auto event = IEventBusPush::MakeEventValue();
    event->data_value.set_intvalue(1);
    batch.values["test:signal"].push_back(event);

    ASSERT_TRUE(controller->push(std::move(batch)));
    ASSERT_TRUE(waitForCount(service.stream_count, 1, std::chrono::milliseconds(1000)));
    ASSERT_TRUE(waitForCount(service.request_count, 1, std::chrono::milliseconds(1000)));

    ASSERT_TRUE(waitForCount(service.stream_close_count, 1, std::chrono::milliseconds(1000)));

    IEventBusPush::EventBatch batch2;
    batch2.root_source = "test-root";
    batch2.tags = {"test"};
    auto event2 = IEventBusPush::MakeEventValue();
    event2->data_value.set_intvalue(2);
    batch2.values["test:signal"].push_back(event2);

    ASSERT_TRUE(controller->push(std::move(batch2)));
    ASSERT_TRUE(waitForCount(service.stream_count, 2, std::chrono::milliseconds(1000)));
    ASSERT_TRUE(waitForCount(service.request_count, 2, std::chrono::milliseconds(1000)));

    controller->stop();
    server->Shutdown();
}

} // namespace mldp_pvxs_driver::controller
