#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <grpcpp/grpcpp.h>
#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>
#include <pool/MLDPGrpcPool.h>
#include <query.grpc.pb.h>

#include "../config/test_config_helpers.h"
#include "../mock/sioc.h"

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace mldp_pvxs_driver::util::pool;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::config::makeConfigFromYaml;

static MLDPGrpcPoolConfig make_pool_config(int min_conn, int max_conn, std::string_view test_provider_id = "test_provider", std::string_view description = "test_provider_desc")
{
    std::ostringstream yaml;
    yaml << "provider_name: " << test_provider_id << "\n"
         << "provider_description: " << description << "\n"
         << "url: dp-ingestion:50051\n"
         << "min_conn: " << min_conn << "\n"
         << "max_conn: " << max_conn << "\n";
    return MLDPGrpcPoolConfig(makeConfigFromYaml(yaml.str()));
}

namespace {

constexpr std::string_view kEpicsIntegrationConfig = R"(
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
          - name: test:voltage
          - name: test:status
          - name: test:waveform
          - name: test:table
          - name: test:bsas_table
            option:
              type: slac-bsas-table
              tsSeconds: secondsPastEpoch
              tsNanos: nanoseconds
)";

constexpr auto kSubscribeTimeout = std::chrono::seconds(10);

class MLDPGrpcPoolIntegrationTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        const auto config = makeConfigFromYaml(std::string(kEpicsIntegrationConfig));
        ASSERT_TRUE(config.valid());

        pvServer_ = std::make_unique<PVServer>();
        controller_ = MLDPPVXSController::create(config);
        ASSERT_TRUE(controller_);
        controller_->start();

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    static void TearDownTestSuite()
    {
        if (controller_)
        {
            controller_->stop();
            controller_.reset();
        }
        pvServer_.reset();
    }

    static std::optional<std::unordered_map<std::string, DataColumn>> queryAndCollectColumns(
        const std::vector<std::string>& pvNames,
        std::chrono::milliseconds       timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto channel = grpc::CreateChannel("dp-query:50052", grpc::InsecureChannelCredentials());
        auto       stub = dp::service::query::DpQueryService::NewStub(channel);

        if (!stub)
        {
            return std::nullopt;
        }

        std::unordered_set<std::string> nameSet(pvNames.begin(), pvNames.end());

        while (std::chrono::steady_clock::now() < deadline)
        {
            dp::service::query::QueryDataRequest request;
            auto*                                spec = request.mutable_queryspec();
            for (const auto& pvName : pvNames)
            {
                spec->add_pvnames(pvName);
            }

            const auto now = std::chrono::system_clock::now();
            const auto begin = now - std::chrono::seconds(30);
            const auto end = now + std::chrono::seconds(1);

            auto* beginTs = spec->mutable_begintime();
            beginTs->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(begin.time_since_epoch()).count());

            auto* endTs = spec->mutable_endtime();
            endTs->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(end.time_since_epoch()).count());

            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

            dp::service::query::QueryDataResponse response;
            const auto status = stub->queryData(&context, request, &response);
            if (status.ok() && response.has_querydata() && !response.has_exceptionalresult())
            {
                std::unordered_map<std::string, DataColumn> collected;
                for (const auto& bucket : response.querydata().databuckets())
                {
                    if (!bucket.has_datacolumn())
                    {
                        continue;
                    }

                    const auto& column = bucket.datacolumn();
                    if (!nameSet.contains(column.name()))
                    {
                        continue;
                    }

                    collected.emplace(column.name(), column);
                    if (collected.size() == nameSet.size())
                    {
                        return collected;
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        return std::nullopt;
    }

    static std::unique_ptr<PVServer>             pvServer_;
    static std::shared_ptr<MLDPPVXSController>   controller_;
};

std::unique_ptr<PVServer>           MLDPGrpcPoolIntegrationTest::pvServer_;
std::shared_ptr<MLDPPVXSController> MLDPGrpcPoolIntegrationTest::controller_;

} // namespace

TEST(MLDPGrpcPoolTest, AcquireBlocksUntilReleased)
{
    // Scope: Verifies that when the pool has a single object (min=max=1),
    // a second caller blocks in `acquire()` until the first holder
    // releases the object. This ensures the pool enforces exclusive use
    // of pooled objects and that `acquire()` correctly waits for
    // availability.

    // Note: the test uses real grpc::Channel objects created with
    // InsecureChannelCredentials but does not depend on a server being
    // available; we only check acquisition/release semantics and pointer
    // identities.

    auto pool = MLDPGrpcPool::create(make_pool_config(1, 1));

    // Acquire first handle
    auto h1 = pool->acquire();
    ASSERT_TRUE(h1);

    std::atomic<bool>  acquired{false};
    std::promise<void> releaseSignal;

    auto fut = std::async(std::launch::async, [&]()
                          {
                              auto h2 = pool->acquire();
                              acquired.store(true);
                              // hold h2 until signaled
                              releaseSignal.get_future().wait();
                              return;
                          });

    // short wait: second acquire should be blocked while h1 holds the object
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(acquired.load());

    // release first handle
    h1.reset();

    // Wait until we observe the pool reports an available object (release observed),
    // or the background thread has acquired it. Poll with timeout to avoid hangs.
    bool releasedSeen = false;
    for (int i = 0; i < 50; ++i)
    {
        if (pool->available() > 0)
        {
            releasedSeen = true;
            break;
        }
        if (acquired.load())
        {
            releasedSeen = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(releasedSeen) << "Expected pool to report available object after release";

    // wait for thread to acquire (if it hasn't yet)
    for (int i = 0; i < 50 && !acquired.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_TRUE(acquired.load());

    // signal thread to release and join
    releaseSignal.set_value();
    fut.get();
}

TEST(MLDPGrpcPoolTest, MultipleObjectsHaveSeparateChannels)
{
    // Scope: Verifies that when the pool is allowed to grow (max_size=2),
    // two concurrently-acquired pooled objects each own distinct
    // `grpc::Channel` instances. This confirms the pool creates separate
    // transport connections for separate pooled objects (transport
    // isolation).

    // Note: we only compare channel pointer identities; the test does not
    // require a live gRPC server.
    auto pool = MLDPGrpcPool::create(make_pool_config(1, 2));
    {
        auto h1 = pool->acquire();
        ASSERT_TRUE(h1);

        auto h2 = pool->acquire();
        ASSERT_TRUE(h2);

        // Channels should be different objects (separate connections)
        EXPECT_NE(h1->channel.get(), h2->channel.get());

        // avalable must be 0 now
        EXPECT_EQ(pool->available(), 0u);
    }
    // cleanup: handles go out of scope and return to pool
    // available must be 2 now
    EXPECT_EQ(pool->available(), 2u);
}

TEST(MLDPGrpcPoolTest, UpdatesMetricsWhenConnectionsMove)
{
    auto        metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    auto        pool = MLDPGrpcPool::create(make_pool_config(1, 1, "test_prv_1"), metrics);
    const auto& providerId = pool->providerId();
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(), 1.0);
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(), 0.0);

    {
        auto handle = pool->acquire();
        ASSERT_TRUE(handle);
        EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(), 0.0);
        EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(), 1.0);

        // send fake data
        dp::service::ingestion::IngestDataRequest  request;
        dp::service::ingestion::IngestDataResponse response;
        request.set_providerid(providerId);
        request.set_clientrequestid("req_123");
        request.add_tags("pv1");
        auto* frame = request.mutable_ingestiondataframe();
        auto* column = frame->add_datacolumns();
        column->set_name("pv1");
        auto* value = column->add_datavalues();
        value->set_intvalue(42);

        auto*      timestamps = frame->mutable_datatimestamps();
        auto*      timestampList = timestamps->mutable_timestamplist();
        auto*      ts = timestampList->add_timestamps();
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        ts->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(now).count());

        {
            grpc::ClientContext context;
            const auto          status = handle->stub->ingestData(&context, request, &response);
            EXPECT_TRUE(status.ok());
            // chekc the response
            EXPECT_TRUE(response.has_ackresult());
            EXPECT_EQ(response.ackresult().numrows(), 1);
            EXPECT_FALSE(response.has_exceptionalresult());
        }
    }

    EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(), 1.0);
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(), 0.0);
}

TEST_F(MLDPGrpcPoolIntegrationTest, QueryCounterPV)
{
    const auto result = queryAndCollectColumns({"test:counter"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& column = result->at("test:counter");
    ASSERT_GT(column.datavalues_size(), 0);
    const auto& value = column.datavalues(0);
    EXPECT_TRUE(value.has_intvalue());
    EXPECT_GT(value.intvalue(), 0);
}

TEST_F(MLDPGrpcPoolIntegrationTest, QueryVoltagePV)
{
    const auto result = queryAndCollectColumns({"test:voltage"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& column = result->at("test:voltage");
    ASSERT_GT(column.datavalues_size(), 0);
    const auto& value = column.datavalues(0);
    EXPECT_TRUE(value.has_doublevalue());
    EXPECT_GE(value.doublevalue(), 0.4);
    EXPECT_LE(value.doublevalue(), 2.6);
}

TEST_F(MLDPGrpcPoolIntegrationTest, QueryStatusPV)
{
    const auto result = queryAndCollectColumns({"test:status"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& column = result->at("test:status");
    ASSERT_GT(column.datavalues_size(), 0);
    const auto& value = column.datavalues(0);
    EXPECT_TRUE(value.has_stringvalue());
    const auto& status = value.stringvalue();
    EXPECT_TRUE(status == "OK" || status == "WARNING" || status == "FAULT");
}

TEST_F(MLDPGrpcPoolIntegrationTest, QueryWaveformPV)
{
    const auto result = queryAndCollectColumns({"test:waveform"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& column = result->at("test:waveform");
    ASSERT_GT(column.datavalues_size(), 0);
    const auto& value = column.datavalues(0);
    ASSERT_TRUE(value.has_arrayvalue());

    const auto& arrayValues = value.arrayvalue().datavalues();
    EXPECT_EQ(arrayValues.size(), 256);
    for (const auto& entry : arrayValues)
    {
        EXPECT_TRUE(entry.has_doublevalue());
        EXPECT_GE(entry.doublevalue(), 0.4);
        EXPECT_LE(entry.doublevalue(), 2.6);
    }
}

TEST_F(MLDPGrpcPoolIntegrationTest, QueryTablePV)
{
    const auto result = queryAndCollectColumns({"test:table"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& column = result->at("test:table");
    ASSERT_GT(column.datavalues_size(), 0);
    const auto& value = column.datavalues(0);
    ASSERT_TRUE(value.has_structurevalue());

    const auto& structure = value.structurevalue();
    const Structure::Field* deviceField = nullptr;
    const Structure::Field* pressureField = nullptr;
    for (const auto& field : structure.fields())
    {
        if (field.name() == "deviceIDs")
        {
            deviceField = &field;
        }
        else if (field.name() == "pressure")
        {
            pressureField = &field;
        }
    }

    ASSERT_NE(deviceField, nullptr);
    ASSERT_NE(pressureField, nullptr);
    ASSERT_TRUE(deviceField->value().has_arrayvalue());
    ASSERT_TRUE(pressureField->value().has_arrayvalue());

    const auto& devices = deviceField->value().arrayvalue().datavalues();
    const auto& pressures = pressureField->value().arrayvalue().datavalues();
    EXPECT_EQ(devices.size(), 3);
    EXPECT_EQ(pressures.size(), 3);
    ASSERT_EQ(devices.size(), pressures.size());

    EXPECT_TRUE(devices[0].has_stringvalue());
    EXPECT_TRUE(devices[1].has_stringvalue());
    EXPECT_TRUE(devices[2].has_stringvalue());
    EXPECT_EQ(devices[0].stringvalue(), "Device A");
    EXPECT_EQ(devices[1].stringvalue(), "Device B");
    EXPECT_EQ(devices[2].stringvalue(), "Device C");

    for (const auto& entry : pressures)
    {
        EXPECT_TRUE(entry.has_doublevalue());
        EXPECT_GE(entry.doublevalue(), -1.6);
        EXPECT_LE(entry.doublevalue(), 1.6);
    }
}

TEST_F(MLDPGrpcPoolIntegrationTest, QueryBsasTablePV)
{
    const auto result = queryAndCollectColumns({"PV_NAME_A_DOUBLE_VALUE", "PV_NAME_B_STRING_VALUE"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& doubleColumn = result->at("PV_NAME_A_DOUBLE_VALUE");
    const auto& stringColumn = result->at("PV_NAME_B_STRING_VALUE");

    ASSERT_EQ(doubleColumn.datavalues_size(), 3);
    EXPECT_TRUE(doubleColumn.datavalues(0).has_doublevalue());
    EXPECT_TRUE(doubleColumn.datavalues(1).has_doublevalue());
    EXPECT_TRUE(doubleColumn.datavalues(2).has_doublevalue());
    EXPECT_EQ(doubleColumn.datavalues(0).doublevalue(), 1.0);
    EXPECT_EQ(doubleColumn.datavalues(1).doublevalue(), 2.0);
    EXPECT_EQ(doubleColumn.datavalues(2).doublevalue(), 3.0);

    ASSERT_EQ(stringColumn.datavalues_size(), 3);
    EXPECT_TRUE(stringColumn.datavalues(0).has_stringvalue());
    EXPECT_TRUE(stringColumn.datavalues(1).has_stringvalue());
    EXPECT_TRUE(stringColumn.datavalues(2).has_stringvalue());
    EXPECT_EQ(stringColumn.datavalues(0).stringvalue(), "OK");
    EXPECT_EQ(stringColumn.datavalues(1).stringvalue(), "WARNING");
    EXPECT_EQ(stringColumn.datavalues(2).stringvalue(), "FAULT");
}
