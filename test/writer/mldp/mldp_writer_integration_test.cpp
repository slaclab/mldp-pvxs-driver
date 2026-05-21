#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <ingestion.grpc.pb.h>
#include <metrics/Metrics.h>
#include <metrics/MetricsConfig.h>
#include <pool/MLDPGrpcPool.h>
#include <query.grpc.pb.h>
#include <query/IQueryable.h>
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <writer/WriterFactory.h>

#include "../../common/MldpQueryTestUtils.h"
#include "../../config/test_config_helpers.h"
#include "../../mock/sioc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::testutil;
using dp::service::common::Structure;
using mldp_pvxs_driver::config::Config;
using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::controller::MLDPPVXSController;
using mldp_pvxs_driver::query::QueryableFactory;
using mldp_pvxs_driver::query::impl::mldp::MLDPQueryClient;

static MLDPGrpcPoolConfig make_pool_config(int min_conn, int max_conn, std::string_view test_provider_id = "test_provider", std::string_view description = "test_provider_desc")
{
    std::ostringstream yaml;
    yaml << "provider-name: " << test_provider_id << "\n"
         << "provider-description: " << description << "\n"
         << "ingestion-url: dp-ingestion:50051\n"
         << "query-url: dp-query:50052\n"
         << "min-conn: " << min_conn << "\n"
         << "max-conn: " << max_conn << "\n";
    return MLDPGrpcPoolConfig(makeConfigFromYaml(yaml.str()));
}

static Config make_pool_config_as_config(int                min_conn,
                                         int                max_conn,
                                         const std::string& provider_name,
                                         const std::string& provider_desc)
{
    std::ostringstream yaml;
    yaml << "provider-name: " << provider_name << "\n"
         << "provider-description: " << provider_desc << "\n"
         << "ingestion-url: dp-ingestion:50051\n"
         << "query-url: dp-query:50052\n"
         << "min-conn: " << min_conn << "\n"
         << "max-conn: " << max_conn << "\n";
    return makeConfigFromYaml(yaml.str());
}

namespace {

constexpr auto kSubscribeTimeout = std::chrono::seconds(10);

std::optional<int64_t> firstIntegerValue(const dp::service::common::DataValues& values)
{
    using namespace mldp_pvxs_driver::testutil;
    const auto col = extractColumn(values);
    if (std::holds_alternative<std::vector<int32_t>>(col.values))
    {
        const auto& v = std::get<std::vector<int32_t>>(col.values);
        if (!v.empty()) return static_cast<int64_t>(v[0]);
    }
    else if (std::holds_alternative<std::vector<int64_t>>(col.values))
    {
        const auto& v = std::get<std::vector<int64_t>>(col.values);
        if (!v.empty()) return v[0];
    }
    return std::nullopt;
}

class MLDPWriterIntegrationTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        pvServer_ = std::make_unique<PVServer>();
        QueryableFactory::instance().reset();
        QueryableFactory::instance().prepare<MLDPQueryClient>(
            make_pool_config_as_config(1, 1, "query_api_probe_provider", "query api probe provider"));
    }

    static void TearDownTestSuite()
    {
        QueryableFactory::instance().reset();
        pvServer_.reset();
    }

    void TearDown() override
    {
        stopController();
    }

    void startControllerWithNoReaders()
    {
        startControllerWithEpicsPVs({"test:counter"});
    }

    void startControllerWithEpicsPVs(std::initializer_list<std::string_view> pv_names)
    {
        std::ostringstream reader_section;
        reader_section << "reader:\n"
                       << "  - epics-pvxs:\n"
                       << "      - name: epics_reader_1\n"
                       << "        pvs:\n";
        for (const auto& pv_name : pv_names)
        {
            reader_section << "          - name: " << pv_name << "\n";
        }
        startControllerWithReaderSection(reader_section.str());
    }

    void startControllerWithBsasTableReader()
    {
        const std::string reader_section =
            "reader:\n" "  - epics-pvxs:\n" "      - name: epics_reader_1\n" "        pvs:\n" "          - name: test:bsas_table\n" "            option:\n" "              type: slac-bsas-table\n" "              tsSeconds: secondsPastEpoch\n" "              tsNanos: nanoseconds\n";
        startControllerWithReaderSection(reader_section);
    }

    void startControllerWithReaderSection(const std::string& reader_section)
    {
        stopController();

        std::ostringstream yaml;
        yaml << "writer:\n"
             << "  mldp:\n"
             << "    - name: mldp_main\n"
             << "      mldp-pool:\n"
             << "        provider-name: test_provider\n"
             << "        ingestion-url: dp-ingestion:50051\n"
             << "        query-url: dp-query:50052\n"
             << "        min-conn: 1\n"
             << "        max-conn: 1\n"
             << reader_section;

        const auto config = makeConfigFromYaml(yaml.str());
        ASSERT_TRUE(config.valid());
        controller_ = MLDPPVXSController::create(config);
        ASSERT_TRUE(controller_);
        controller_->start();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    void stopController()
    {
        if (!controller_)
        {
            return;
        }
        controller_->stop();
        controller_.reset();
    }

    static std::unique_ptr<PVServer>    pvServer_;
    std::shared_ptr<MLDPPVXSController> controller_;
};

std::unique_ptr<PVServer> MLDPWriterIntegrationTest::pvServer_;

} // namespace

TEST(MLDPWriterPoolTest, AcquireBlocksUntilReleased)
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

TEST(MLDPWriterPoolTest, MultipleObjectsHaveSeparateChannels)
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

TEST(MLDPWriterPoolTest, UpdatesMetricsWhenConnectionsMove)
{
    const prometheus::Labels ingestionPoolLabel{{"pool", "ingestion"}};
    auto                     metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    auto                     pool = MLDPGrpcPool::create(make_pool_config(1, 1, "test_prv_1"), metrics);
    const auto&              providerId = pool->providerId();
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(ingestionPoolLabel), 1.0);
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(ingestionPoolLabel), 0.0);

    {
        auto handle = pool->acquire();
        ASSERT_TRUE(handle);
        EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(ingestionPoolLabel), 0.0);
        EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(ingestionPoolLabel), 1.0);

        // send fake data
        dp::service::ingestion::IngestDataRequest  request;
        dp::service::ingestion::IngestDataResponse response;
        request.set_providerid(providerId);
        request.set_clientrequestid("req_123");
        auto* frame = request.mutable_ingestiondataframe();
        auto* column = frame->add_int32columns();
        column->set_name("pv1");
        column->add_values(42);

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

    EXPECT_DOUBLE_EQ(metrics->poolConnectionsAvailable(ingestionPoolLabel), 1.0);
    EXPECT_DOUBLE_EQ(metrics->poolConnectionsInUse(ingestionPoolLabel), 0.0);
}

TEST_F(MLDPWriterIntegrationTest, QueryCounterPV)
{
    startControllerWithEpicsPVs({"test:counter"});

    const auto result = queryAndCollectColumns({"test:counter"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& buckets = result->at("test:counter");
    const auto  cols = flattenColumns(buckets);
    ASSERT_GT(cols.size(), 0);
    const auto& col = cols[0];
    EXPECT_TRUE(std::holds_alternative<std::vector<int32_t>>(col.values) ||
                std::holds_alternative<std::vector<int64_t>>(col.values));
    if (std::holds_alternative<std::vector<int32_t>>(col.values))
    {
        ASSERT_FALSE(std::get<std::vector<int32_t>>(col.values).empty());
        EXPECT_GT(std::get<std::vector<int32_t>>(col.values)[0], 0);
    }
    else
    {
        ASSERT_FALSE(std::get<std::vector<int64_t>>(col.values).empty());
        EXPECT_GT(std::get<std::vector<int64_t>>(col.values)[0], 0LL);
    }
}

TEST_F(MLDPWriterIntegrationTest, QueryVoltagePV)
{
    startControllerWithEpicsPVs({"test:voltage"});

    const auto result = queryAndCollectColumns({"test:voltage"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& buckets = result->at("test:voltage");
    const auto  cols = flattenColumns(buckets);
    ASSERT_GT(cols.size(), 0);
    const auto& col = cols[0];
    ASSERT_TRUE(std::holds_alternative<std::vector<double>>(col.values));
    const auto& vals = std::get<std::vector<double>>(col.values);
    ASSERT_FALSE(vals.empty());
    EXPECT_GE(vals[0], 0.4);
    EXPECT_LE(vals[0], 2.6);
}

TEST_F(MLDPWriterIntegrationTest, QueryStatusPV)
{
    startControllerWithEpicsPVs({"test:status"});

    const auto result = queryAndCollectColumns({"test:status"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& buckets = result->at("test:status");
    const auto  cols = flattenColumns(buckets);
    ASSERT_GT(cols.size(), 0);
    const auto& col = cols[0];
    ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(col.values));
    const auto& vals = std::get<std::vector<std::string>>(col.values);
    ASSERT_FALSE(vals.empty());
    const auto& status = vals[0];
    EXPECT_TRUE(status == "OK" || status == "WARNING" || status == "FAULT");
}

TEST_F(MLDPWriterIntegrationTest, QueryWaveformPV)
{
    startControllerWithEpicsPVs({"test:waveform"});

    const auto result = queryAndCollectColumns({"test:waveform"}, kSubscribeTimeout);
    ASSERT_TRUE(result.has_value());

    const auto& buckets = result->at("test:waveform");
    const auto  cols = flattenColumns(buckets);
    ASSERT_GT(cols.size(), 0);
    const auto& col = cols[0];
    ASSERT_TRUE(std::holds_alternative<std::vector<std::vector<double>>>(col.values));
    const auto& outer = std::get<std::vector<std::vector<double>>>(col.values);
    ASSERT_FALSE(outer.empty());
    const auto& inner = outer[0];
    EXPECT_EQ(inner.size(), 256u);
    for (const auto& entry : inner)
    {
        EXPECT_GE(entry, 0.4);
        EXPECT_LE(entry, 2.6);
    }
}

// TEST_F(MLDPWriterIntegrationTest, QueryTablePV)
// {
//     startControllerWithEpicsPVs({"test:table"});

// const auto result = queryAndCollectColumns({"test:table"}, kSubscribeTimeout);
// ASSERT_TRUE(result.has_value());

// const auto& buckets = result->at("test:table");
// const auto  rows = flattenDataValues(buckets);
// ASSERT_GT(rows.size(), 0);
// const auto& value = rows[0];
// ASSERT_TRUE(value.has_structurevalue());

// const auto& structure = value.structurevalue();
// const Structure::Field* deviceField = nullptr;
// const Structure::Field* pressureField = nullptr;
// for (const auto& field : structure.fields())
// {
//     if (field.name() == "deviceIDs")
//     {
//         deviceField = &field;
//     }
//     else if (field.name() == "pressure")
//     {
//         pressureField = &field;
//     }
// }

// ASSERT_NE(deviceField, nullptr);
// ASSERT_NE(pressureField, nullptr);
// ASSERT_TRUE(deviceField->value().has_arrayvalue());
// ASSERT_TRUE(pressureField->value().has_arrayvalue());

// const auto& devices = deviceField->value().arrayvalue().datavalues();
// const auto& pressures = pressureField->value().arrayvalue().datavalues();
// EXPECT_EQ(devices.size(), 3);
// EXPECT_EQ(pressures.size(), 3);
// ASSERT_EQ(devices.size(), pressures.size());

// EXPECT_EQ(devices[0].value_case(), DataValue::kStringValue);
// EXPECT_EQ(devices[1].value_case(), DataValue::kStringValue);
// EXPECT_EQ(devices[2].value_case(), DataValue::kStringValue);
// EXPECT_EQ(devices[0].stringvalue(), "Device A");
// EXPECT_EQ(devices[1].stringvalue(), "Device B");
// EXPECT_EQ(devices[2].stringvalue(), "Device C");

// for (const auto& entry : pressures)
// {
//     EXPECT_EQ(entry.value_case(), DataValue::kDoubleValue);
//     EXPECT_GE(entry.doublevalue(), -1.6);
//     EXPECT_LE(entry.doublevalue(), 1.6);
// }
// }

TEST_F(MLDPWriterIntegrationTest, QueryBsasTablePV)
{
    startControllerWithBsasTableReader();

    // PV_A: Float64 column
    const auto resultA = queryAndCollectColumns({"PV_A"}, kSubscribeTimeout);
    ASSERT_TRUE(resultA.has_value()) << "PV_A not found in dp-service";

    const auto colsA = flattenColumns(resultA->at("PV_A"));
    ASSERT_GE(colsA.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<std::vector<double>>(colsA[0].values));
    EXPECT_GE(std::get<std::vector<double>>(colsA[0].values).size(), 3u);
    EXPECT_EQ(colsA[0].provenance_source, "test:bsas_table");

    // PV_B: Int32 column
    const auto resultB = queryAndCollectColumns({"PV_B"}, kSubscribeTimeout);
    ASSERT_TRUE(resultB.has_value()) << "PV_B not found in dp-service";

    const auto colsB = flattenColumns(resultB->at("PV_B"));
    ASSERT_GE(colsB.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<std::vector<int32_t>>(colsB[0].values));
    EXPECT_GE(std::get<std::vector<int32_t>>(colsB[0].values).size(), 3u);
    EXPECT_EQ(colsB[0].provenance_source, "test:bsas_table");

    // PV_C: Float32 column (arrives as floatColumn or doubleColumn depending on server)
    const auto resultC = queryAndCollectColumns({"PV_C"}, kSubscribeTimeout);
    ASSERT_TRUE(resultC.has_value()) << "PV_C not found in dp-service";

    const auto colsC = flattenColumns(resultC->at("PV_C"));
    ASSERT_GE(colsC.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<std::vector<float>>(colsC[0].values) ||
                std::holds_alternative<std::vector<double>>(colsC[0].values));
    EXPECT_EQ(colsC[0].provenance_source, "test:bsas_table");
}

TEST_F(MLDPWriterIntegrationTest, CharacterizesDuplicateIngestBehaviorForSameSample)
{
    auto pool = MLDPGrpcPool::create(make_pool_config(1, 1, "duplicate_probe_provider", "duplicate probe provider"));
    ASSERT_TRUE(pool);

    const auto        providerId = pool->providerId();
    const std::string pvName = "test:duplicate:probe";
    const auto        now = std::chrono::system_clock::now().time_since_epoch();
    const auto        ts_sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto        ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1'000'000'000LL;

    auto buildRequest = [&](std::string client_request_id)
    {
        dp::service::ingestion::IngestDataRequest request;
        request.set_providerid(providerId);
        request.set_clientrequestid(std::move(client_request_id));

        auto* frame = request.mutable_ingestiondataframe();
        auto* column = frame->add_int32columns();
        column->set_name(pvName);
        column->add_values(42);

        auto* timestamps = frame->mutable_datatimestamps();
        auto* list = timestamps->mutable_timestamplist();
        auto* ts = list->add_timestamps();
        ts->set_epochseconds(ts_sec);
        ts->set_nanoseconds(static_cast<uint32_t>(ts_ns));

        return request;
    };

    {
        auto handle = pool->acquire();
        ASSERT_TRUE(handle);

        for (int i = 0; i < 2; ++i)
        {
            auto                                       request = buildRequest("dup_probe_req_" + std::to_string(i));
            dp::service::ingestion::IngestDataResponse response;
            grpc::ClientContext                        context;
            const auto                                 status = handle->stub->ingestData(&context, request, &response);
            ASSERT_TRUE(status.ok());
            ASSERT_TRUE(response.has_ackresult());
            EXPECT_EQ(response.ackresult().numrows(), 1);
            EXPECT_FALSE(response.has_exceptionalresult());
        }
    }

    const auto result = queryAndCollectColumns({pvName}, std::chrono::seconds(10));
    ASSERT_TRUE(result.has_value());
    const auto cols = flattenColumns(result->at(pvName));

    // Characterization test: some MLDP deployments deduplicate identical samples,
    // while others store both rows. This test records the current behavior while
    // still ensuring the query path returns the inserted sample value.
    ASSERT_GE(cols.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<std::vector<int32_t>>(cols[0].values));
    const auto& vals = std::get<std::vector<int32_t>>(cols[0].values);
    ASSERT_GE(vals.size(), 1u);
    ASSERT_LE(vals.size(), 2u);
    for (const auto& v : vals)
    {
        EXPECT_EQ(v, 42);
    }
}

TEST_F(MLDPWriterIntegrationTest, QueryClientReturnsMetadataAndDataForInsertedPV)
{
    startControllerWithNoReaders();
    ASSERT_TRUE(controller_);

    auto pool = MLDPGrpcPool::create(make_pool_config(1, 1, "query_api_probe_provider", "query api probe provider"));
    ASSERT_TRUE(pool);
    const auto provider_id = pool->providerId();
    ASSERT_FALSE(provider_id.empty());

    const auto        now = std::chrono::system_clock::now().time_since_epoch();
    const auto        ts_sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto        ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1'000'000'000LL;
    const std::string pv_name = "test:query:api:probe:" + std::to_string(ts_sec) + ":" + std::to_string(ts_ns);
    constexpr int     expected_value = 777;

    dp::service::ingestion::IngestDataRequest request;
    request.set_providerid(provider_id);
    request.set_clientrequestid("query_api_probe_req");
    auto* frame = request.mutable_ingestiondataframe();
    auto* column = frame->add_int32columns();
    column->set_name(pv_name);
    column->add_values(expected_value);
    auto* timestamps = frame->mutable_datatimestamps();
    auto* list = timestamps->mutable_timestamplist();
    auto* ts = list->add_timestamps();
    ts->set_epochseconds(ts_sec);
    ts->set_nanoseconds(static_cast<uint32_t>(ts_ns));

    {
        auto handle = pool->acquire();
        ASSERT_TRUE(handle);
        dp::service::ingestion::IngestDataResponse response;
        grpc::ClientContext                        context;
        const auto                                 status = handle->stub->ingestData(&context, request, &response);
        ASSERT_TRUE(status.ok());
        ASSERT_TRUE(response.has_ackresult());
        ASSERT_EQ(response.ackresult().numrows(), 1);
        ASSERT_FALSE(response.has_exceptionalresult());
    }

    auto queryClient = QueryableFactory::instance().create<MLDPQueryClient>();
    const std::set<std::string> sources{pv_name};

    const auto                                                       metadata_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::optional<mldp_pvxs_driver::util::bus::IDataBus::SourceInfo> source_info;
    while (std::chrono::steady_clock::now() < metadata_deadline)
    {
        const auto infos = queryClient->querySourcesInfo(sources);
        const auto it = std::find_if(infos.begin(),
                                     infos.end(),
                                     [&](const auto& info)
                                     {
                                         return info.source_name == pv_name;
                                     });
        if (it != infos.end())
        {
            source_info = *it;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Some deployed MLDP query services do not expose queryPvMetadata RPC.
    // Keep exercising querySourcesInfo API, but don't fail if metadata endpoint is unavailable.
    if (source_info.has_value())
    {
        EXPECT_TRUE(source_info->last_timestamp.has_value());
        if (source_info->last_timestamp.has_value())
        {
            EXPECT_GE(source_info->last_timestamp->epoch_seconds, static_cast<uint64_t>(ts_sec));
        }
    }

    mldp_pvxs_driver::util::bus::QuerySourcesDataOptions options;
    options.timeout = std::chrono::seconds(10);
    options.lookback_window = std::chrono::seconds(120);
    options.forward_window = std::chrono::seconds(5);
    options.rpc_deadline = std::chrono::seconds(5);

    const auto data = queryClient->querySourcesData(sources, options);
    ASSERT_TRUE(data.has_value());
    const auto buckets_it = data->find(pv_name);
    ASSERT_NE(buckets_it, data->end());
    ASSERT_FALSE(buckets_it->second.empty());
    const auto& first_values = buckets_it->second.front();
    const auto  first = firstIntegerValue(first_values);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), expected_value);
}

TEST_F(MLDPWriterIntegrationTest, QueryClientReturnsAllRequestedInsertedPVs)
{
    startControllerWithNoReaders();
    ASSERT_TRUE(controller_);

    auto pool = MLDPGrpcPool::create(make_pool_config(1, 1, "query_data_multi_probe_provider", "query data multi probe provider"));
    ASSERT_TRUE(pool);
    const auto provider_id = pool->providerId();
    ASSERT_FALSE(provider_id.empty());

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ts_sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1'000'000'000LL;

    const std::string pv_a = "test:query:data:multi:a:" + std::to_string(ts_sec) + ":" + std::to_string(ts_ns);
    const std::string pv_b = "test:query:data:multi:b:" + std::to_string(ts_sec) + ":" + std::to_string(ts_ns);
    constexpr int     value_a = 101;
    constexpr int     value_b = 202;

    auto ingest_one = [&](const std::string& pv_name, int value_int, const std::string& request_id)
    {
        dp::service::ingestion::IngestDataRequest request;
        request.set_providerid(provider_id);
        request.set_clientrequestid(request_id);
        auto* frame = request.mutable_ingestiondataframe();
        auto* column = frame->add_int32columns();
        column->set_name(pv_name);
        column->add_values(value_int);
        auto* ts = frame->mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
        ts->set_epochseconds(ts_sec);
        ts->set_nanoseconds(static_cast<uint32_t>(ts_ns));

        auto handle = pool->acquire();
        ASSERT_TRUE(handle);
        dp::service::ingestion::IngestDataResponse response;
        grpc::ClientContext                        context;
        const auto                                 status = handle->stub->ingestData(&context, request, &response);
        ASSERT_TRUE(status.ok());
        ASSERT_TRUE(response.has_ackresult());
        ASSERT_EQ(response.ackresult().numrows(), 1);
        ASSERT_FALSE(response.has_exceptionalresult());
    };

    ingest_one(pv_a, value_a, "query_data_multi_req_a");
    ingest_one(pv_b, value_b, "query_data_multi_req_b");

    auto queryClient = std::make_unique<MLDPQueryClient>(
        make_pool_config(1, 1, "query_data_multi_probe_provider", "query data multi probe provider"));
    const std::set<std::string> sources{pv_a, pv_b};

    mldp_pvxs_driver::util::bus::QuerySourcesDataOptions options;
    options.timeout = std::chrono::seconds(10);
    options.lookback_window = std::chrono::seconds(120);
    options.forward_window = std::chrono::seconds(5);
    options.rpc_deadline = std::chrono::seconds(5);

    const auto data = queryClient->querySourcesData(sources, options);
    ASSERT_TRUE(data.has_value());
    ASSERT_EQ(data->size(), sources.size());

    const auto it_a = data->find(pv_a);
    const auto it_b = data->find(pv_b);
    ASSERT_NE(it_a, data->end());
    ASSERT_NE(it_b, data->end());
    ASSERT_FALSE(it_a->second.empty());
    ASSERT_FALSE(it_b->second.empty());

    const auto& first_a = it_a->second.front();
    const auto& first_b = it_b->second.front();
    const auto  first_int_a = firstIntegerValue(first_a);
    const auto  first_int_b = firstIntegerValue(first_b);
    ASSERT_TRUE(first_int_a.has_value());
    ASSERT_TRUE(first_int_b.has_value());
    EXPECT_EQ(first_int_a.value(), value_a);
    EXPECT_EQ(first_int_b.value(), value_b);
}

// ---------------------------------------------------------------------------
// Helpers for standalone MLDPWriter unit tests (no controller)
// ---------------------------------------------------------------------------

namespace {

class WriterTestIngestionService final
    : public dp::service::ingestion::DpIngestionService::Service
{
public:
    std::atomic<int>                                        request_count{0};
    std::vector<dp::service::ingestion::IngestDataRequest> captured_requests;
    std::mutex                                             captured_mutex;

    grpc::Status registerProvider(
        grpc::ServerContext*,
        const dp::service::ingestion::RegisterProviderRequest*,
        dp::service::ingestion::RegisterProviderResponse* response) override
    {
        auto* result = response->mutable_registrationresult();
        result->set_providerid("writer-test-provider-id");
        result->set_providername("writer-test-provider");
        result->set_isnewprovider(true);
        return grpc::Status::OK;
    }

    grpc::Status ingestDataStream(
        grpc::ServerContext*,
        grpc::ServerReader<dp::service::ingestion::IngestDataRequest>* reader,
        dp::service::ingestion::IngestDataStreamResponse*) override
    {
        dp::service::ingestion::IngestDataRequest request;
        while (reader->Read(&request))
        {
            request_count.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(captured_mutex);
            captured_requests.push_back(request);
        }
        return grpc::Status::OK;
    }
};

bool writerWaitForCount(std::atomic<int>& counter, int target, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (counter.load(std::memory_order_relaxed) >= target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return counter.load(std::memory_order_relaxed) >= target;
}

} // namespace

// ---------------------------------------------------------------------------
// AcceptsOnlyTimeSeriesPayload
// Verify acceptsPayload() type-dispatch without requiring a live server.
// ---------------------------------------------------------------------------

TEST(MLDPWriterTest, AcceptsOnlyTimeSeriesPayload)
{
    // acceptsPayload() is a pure std::holds_alternative check — no network needed.
    static constexpr std::string_view kYaml = R"(
name: mldp_accepts_test
mldp-pool:
  provider-name: test-provider
  ingestion-url: 127.0.0.1:50051
  query-url: 127.0.0.1:50052
  min-conn: 1
  max-conn: 1
)";

    const auto cfg = makeConfigFromYaml(std::string(kYaml));
    ASSERT_TRUE(cfg.valid());

    auto writer = mldp_pvxs_driver::writer::WriterFactory::create("mldp", cfg, nullptr);
    ASSERT_TRUE(writer);

    using namespace mldp_pvxs_driver::util::bus;
    EXPECT_TRUE(writer->acceptsPayload(TimeSeriesPayload{}));
    EXPECT_FALSE(writer->acceptsPayload(SourceMetadataPayload{}));
    EXPECT_FALSE(writer->acceptsPayload(ConfigurationPayload{}));
    EXPECT_FALSE(writer->acceptsPayload(ConfigurationActivationPayload{}));
}

// ---------------------------------------------------------------------------
// BatchMetadataAppearsAsColumnAttributes
// Verify batch.metadata flows through MLDPWriter into gRPC column attributes.
// Tests the writer directly (no controller) to isolate MLDPWriter conversion.
// ---------------------------------------------------------------------------

TEST(MLDPWriterTest, BatchMetadataAppearsAsColumnAttributes)
{
    WriterTestIngestionService service;
    grpc::ServerBuilder        builder;
    int                        port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    ASSERT_TRUE(server);
    ASSERT_GT(port, 0);

    std::ostringstream yaml;
    yaml << "name: mldp_meta_test\n"
         << "mldp-pool:\n"
         << "  provider-name: meta-test-provider\n"
         << "  ingestion-url: 127.0.0.1:" << port << "\n"
         << "  query-url: localhost:" << port << "\n"
         << "  min-conn: 1\n"
         << "  max-conn: 1\n";

    const auto cfg = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(cfg.valid());

    auto writer = mldp_pvxs_driver::writer::WriterFactory::create("mldp", cfg, nullptr);
    ASSERT_TRUE(writer);
    ASSERT_NO_THROW(writer->start());

    using namespace mldp_pvxs_driver::util::bus;

    IDataBus::EventBatch batch;
    batch.root_source = "test:meta:signal";
    batch.metadata    = {{"facility", "lcls"}, {"signal_type", "scalar"}};

    DataBatch  frame;
    frame.timestamps.push_back({1000000000, 0});
    DataColumn col;
    col.name   = "value";
    col.values = std::vector<double>{42.0};
    frame.columns.push_back(std::move(col));

    TimeSeriesPayload ts;
    ts.frames.push_back(std::move(frame));
    batch.payload = std::move(ts);

    ASSERT_TRUE(writer->push(std::move(batch)));
    ASSERT_TRUE(writerWaitForCount(service.request_count, 1, std::chrono::milliseconds(5000)));

    std::vector<dp::service::ingestion::IngestDataRequest> captured;
    {
        std::lock_guard<std::mutex> lock(service.captured_mutex);
        captured = service.captured_requests;
    }
    ASSERT_FALSE(captured.empty()) << "No IngestDataRequest captured from MLDPWriter";

    const auto hasAttr = [](const google::protobuf::RepeatedPtrField<dp::service::common::Attribute>& attrs,
                             const std::string& key,
                             const std::string& value) -> bool
    {
        for (const auto& a : attrs)
        {
            if (a.name() == key && a.value() == value)
                return true;
        }
        return false;
    };

    bool checked = false;
    for (const auto& req : captured)
    {
        const auto& df = req.ingestiondataframe();
        for (int i = 0; i < df.doublecolumns_size(); ++i)
        {
            const auto& attrs = df.doublecolumns(i).metadata().attributes();
            EXPECT_TRUE(hasAttr(attrs, "facility",    "lcls"))
                << "doublecolumns[" << i << "] missing facility attribute";
            EXPECT_TRUE(hasAttr(attrs, "signal_type", "scalar"))
                << "doublecolumns[" << i << "] missing signal_type attribute";
            EXPECT_EQ(df.doublecolumns(i).metadata().provenance().source(), "test:meta:signal")
                << "doublecolumns[" << i << "] provenance.source mismatch";
            checked = true;
        }
    }
    EXPECT_TRUE(checked) << "No double columns found in captured requests to verify";

    writer->stop();
    server->Shutdown();
}
