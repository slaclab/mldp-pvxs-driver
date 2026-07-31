//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <annotation.grpc.pb.h>
#include <config/Config.h>
#include <controller/MLDPPVXSController.h>
#include <query/ExecutionContext.h>
#include <query/QueryExecutor.h>
#include <query/QueryPlanner.h>
#include <query/QuerySubcommand.h>
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <query/parser/QueryParser.h>
#include <util/bus/IDataBus.h>
#include <writer/WriterFactory.h>

#include <arrow/array.h>
#include <arrow/type.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "../config/test_config_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::controller;

namespace {

constexpr auto       kPollDeadline = std::chrono::seconds(30);
constexpr auto       kPollInterval = std::chrono::milliseconds(250);
constexpr uint32_t   kPageSize = 1;
std::atomic_uint64_t gNamespaceSuffix{0};

std::string quote(const std::string& value)
{
    return "'" + value + "'";
}

std::string commaSeparatedQuoted(const std::vector<std::string>& values)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << quote(values[i]);
    }
    return out.str();
}

std::string makeNamespace()
{
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return "query_it_" + std::to_string(now) + "_" +
           std::to_string(gNamespaceSuffix.fetch_add(1, std::memory_order_relaxed));
}

config::Config makeQueryConfig(const uint32_t max_connections = 1)
{
    return config::makeConfigFromYaml(
        "provider-name: queryable-mldp-integration\n" "provider-description: real MLDP query integration test\n" "ingestion-url: dp-ingestion:50051\n" "query-url: dp-query:50052\n" "annotation-url: dp-annotation:50053\n" "min-conn: 1\n" "max-conn: " + std::to_string(max_connections) + "\n");
}

std::string makeMldpWriterConfig(const std::string& name)
{
    return "name: " + name + "\n" "thread-pool: 1\n" "stream-max-age-ms: 1\n" "mldp-pool:\n" "  provider-name: " + name + "\n" "  provider-description: real MLDP query integration test\n" "  ingestion-url: dp-ingestion:50051\n" "  query-url: dp-query:50052\n" "  min-conn: 1\n" "  max-conn: 1\n";
}

std::string makeAnnotationWriterConfig(const std::string& name, const std::string& pool_key)
{
    return "name: " + name + "\n" "thread-pool: 1\n" "deadline-seconds: 10\n" +
           pool_key + ":\n" "  provider-name: " + name + "\n" "  provider-description: real MLDP query integration test\n" "  ingestion-url: dp-ingestion:50051\n" "  query-url: dp-query:50052\n" "  annotation-url: dp-annotation:50053\n" "  min-conn: 1\n" "  max-conn: 1\n";
}

std::string makeControllerConfig(const std::string& name)
{
    return "name: " + name + "\n" "writer:\n" "  mldp:\n" "    - name: " + name + "_time_series\n" "      thread-pool: 1\n" "      stream-max-age-ms: 1\n" "      mldp-pool:\n" "        provider-name: " + name + "\n" "        provider-description: controller wide-table integration test\n" "        ingestion-url: dp-ingestion:50051\n" "        query-url: dp-query:50052\n" "        min-conn: 1\n" "        max-conn: 1\n" "  mldp-pv-metadata:\n" "    - name: " + name + "_metadata\n" "      thread-pool: 1\n" "      deadline-seconds: 10\n" "      mldp-pv-metadata-pool:\n" "        provider-name: " + name + "\n" "        provider-description: controller wide-table integration test\n" "        ingestion-url: dp-ingestion:50051\n" "        query-url: dp-query:50052\n" "        annotation-url: dp-annotation:50053\n" "        min-conn: 1\n" "        max-conn: 1\n" "  mldp-configuration:\n" "    - name: " + name + "_configuration\n" "      thread-pool: 1\n" "      deadline-seconds: 10\n" "      mldp-annotation-pool:\n" "        provider-name: " + name + "\n" "        provider-description: controller wide-table integration test\n" "        ingestion-url: dp-ingestion:50051\n" "        query-url: dp-query:50052\n" "        annotation-url: dp-annotation:50053\n" "        min-conn: 1\n" "        max-conn: 1\n" "reader:\n" "  epics-pvxs:\n" "    - name: controller-wide-reader\n" "      pvs:\n" "        - name: test:counter\n";
}

class QueryableMldpIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        nameSpace_ = makeNamespace();
        QueryableFactory::instance().reset();
        const auto query_config = makeQueryConfig();
        QueryableFactory::instance().prepare<MLDPQueryClient>(query_config);
        QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(query_config);
        annotation_stub_ = dp::service::annotation::DpAnnotationService::NewStub(
            grpc::CreateChannel("dp-annotation:50053", grpc::InsecureChannelCredentials()));
    }

    void TearDown() override
    {
        if (controller_)
        {
            controller_->stop();
            controller_.reset();
        }
        cleanupAnnotations();
        QueryableFactory::instance().reset();
    }

    void configureQueryables(const uint32_t max_connections)
    {
        QueryableFactory::instance().reset();
        const auto query_config = makeQueryConfig(max_connections);
        QueryableFactory::instance().prepare<MLDPQueryClient>(query_config);
        QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(query_config);
    }

    void startController()
    {
        controller_ = MLDPPVXSController::create(config::makeConfigFromYaml(makeControllerConfig(nameSpace_)));
        ASSERT_NE(controller_, nullptr);
        ASSERT_NO_THROW(controller_->start());
    }

    std::string pv(const std::string& suffix) const
    {
        return nameSpace_ + ":" + suffix;
    }

    std::string configName(const std::string& suffix) const
    {
        return nameSpace_ + "_" + suffix;
    }

    QueryExecutionResult executeSql(const std::string& sql, uint32_t page_size = kPageSize) const
    {
        QueryPlanner  planner;
        QueryExecutor executor;
        return executor.execute(planner.plan(parseQuery(sql)), ExecutionContext{
                                                                   .pool = arrow::default_memory_pool(),
                                                                   .join_batch_size = page_size,
                                                               });
    }

    QueryExecutionResult pollSql(const std::string&                                      query,
                                 const std::string&                                      missing_record_type,
                                 const std::function<bool(const QueryExecutionResult&)>& visible,
                                 uint32_t                                                page_size = kPageSize) const
    {
        const auto  deadline = std::chrono::steady_clock::now() + kPollDeadline;
        std::string last_error;
        while (std::chrono::steady_clock::now() < deadline)
        {
            try
            {
                auto result = executeSql(query, page_size);
                if (visible(result))
                {
                    return result;
                }
            }
            catch (const std::exception& error)
            {
                last_error = error.what();
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        ADD_FAILURE() << "Timed out waiting for " << missing_record_type << " in namespace '"
                      << nameSpace_ << "' through SQL query: " << query
                      << (last_error.empty() ? "" : "; last error: " + last_error);
        return {};
    }

    void seedTimeSeries(const std::string& source_pv, int count, int64_t base_value)
    {
        auto writer = WriterFactory::create("mldp", config::makeConfigFromYaml(makeMldpWriterConfig(nameSpace_ + "_mldp")), nullptr);
        ASSERT_NE(writer, nullptr);
        ASSERT_NO_THROW(writer->start());

        DataBatch  frame;
        const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        for (int index = 0; index < count; ++index)
        {
            frame.timestamps.push_back(TimestampEntry{
                .epoch_seconds = static_cast<uint64_t>(now_seconds + index),
                .nanoseconds = static_cast<uint64_t>(index)});
        }
        frame.columns.push_back(DataColumn{
            .name = source_pv,
            .values = [&]()
            {
                std::vector<int64_t> values;
                values.reserve(count);
                for (int index = 0; index < count; ++index)
                {
                    values.push_back(base_value + index);
                }
                return values;
            }(),
        });

        IDataBus::EventBatch batch;
        batch.payload = TimeSeriesPayload{
            .root_source_name = source_pv,
            .frames = {std::move(frame)},
        };
        ASSERT_TRUE(writer->push(std::move(batch)));
        writer->stop();
    }

    void seedMetadata(const std::vector<std::string>& pvs)
    {
        auto writer = WriterFactory::create(
            "mldp-pv-metadata",
            config::makeConfigFromYaml(makeAnnotationWriterConfig(nameSpace_ + "_metadata", "mldp-pv-metadata-pool")),
            nullptr);
        ASSERT_NE(writer, nullptr);
        ASSERT_NO_THROW(writer->start());

        SourceMetadataPayload payload;
        payload.root_source_name = nameSpace_;
        for (const auto& source_pv : pvs)
        {
            payload.sources.emplace(source_pv, SourceMetadataEntry{
                                                   .aliases = std::vector<std::string>{source_pv + ":alias"},
                                                   .tags = std::vector<std::string>{nameSpace_, "magnet"},
                                                   .attributes = {{"namespace", nameSpace_}},
                                                   .description = "query integration metadata",
                                                   .modified_by = "queryable_mldp_integration_test",
                                               });
            metadataPvs_.push_back(source_pv);
        }

        IDataBus::EventBatch batch;
        batch.payload = std::move(payload);
        ASSERT_TRUE(writer->push(std::move(batch)));
        writer->stop();
    }

    void seedConfiguration(const std::string&                           name,
                           const std::string&                           category,
                           const std::string&                           activation_id,
                           const BusTimestamp&                          start_time,
                           std::optional<BusTimestamp>                  end_time = std::nullopt,
                           std::optional<std::vector<std::string>>      activation_tags = std::nullopt,
                           std::unordered_map<std::string, std::string> activation_attributes = {})
    {
        auto writer = WriterFactory::create(
            "mldp-configuration",
            config::makeConfigFromYaml(makeAnnotationWriterConfig(nameSpace_ + "_configuration", "mldp-annotation-pool")),
            nullptr);
        ASSERT_NE(writer, nullptr);
        ASSERT_NO_THROW(writer->start());

        IDataBus::EventBatch configuration_batch;
        configuration_batch.payload = ConfigurationPayload{
            .root_source_name = nameSpace_,
            .configuration_name = name,
            .category = category,
            .description = "query integration configuration",
            .attributes = {{"namespace", nameSpace_}},
            .modified_by = "queryable_mldp_integration_test",
        };
        ASSERT_TRUE(writer->push(std::move(configuration_batch)));

        activation_attributes.insert({"namespace", nameSpace_});
        IDataBus::EventBatch activation_batch;
        activation_batch.payload = ConfigurationActivationPayload{
            .client_activation_id = activation_id,
            .configuration_name = name,
            .start_time = start_time,
            .end_time = end_time,
            .description = "query integration activation",
            .tags = std::move(activation_tags),
            .attributes = std::move(activation_attributes),
            .modified_by = "queryable_mldp_integration_test",
        };
        ASSERT_TRUE(writer->push(std::move(activation_batch)));
        writer->stop();
        configurationNames_.push_back(name);
        activationIds_.push_back(activation_id);
    }

    static int64_t rowCount(const QueryExecutionResult& result)
    {
        int64_t count = 0;
        for (const auto& batch : result.batches)
        {
            count += batch->num_rows();
        }
        return count;
    }

    static std::vector<std::string> strings(const QueryExecutionResult& result, int column)
    {
        std::vector<std::string> values;
        for (const auto& batch : result.batches)
        {
            const auto array = std::static_pointer_cast<arrow::StringArray>(batch->column(column));
            for (int64_t index = 0; index < array->length(); ++index)
            {
                if (!array->IsNull(index))
                {
                    values.push_back(array->GetString(index));
                }
            }
        }
        return values;
    }

    void cleanupAnnotations() noexcept
    {
        if (!annotation_stub_)
        {
            return;
        }
        for (const auto& activation_id : activationIds_)
        {
            dp::service::annotation::DeleteConfigurationActivationRequest request;
            request.set_clientactivationid(activation_id);
            dp::service::annotation::DeleteConfigurationActivationResponse response;
            grpc::ClientContext                                            context;
            annotation_stub_->deleteConfigurationActivation(&context, request, &response);
        }
        for (const auto& name : configurationNames_)
        {
            dp::service::annotation::DeleteConfigurationRequest request;
            request.set_configurationname(name);
            dp::service::annotation::DeleteConfigurationResponse response;
            grpc::ClientContext                                  context;
            annotation_stub_->deleteConfiguration(&context, request, &response);
        }
        for (const auto& source_pv : metadataPvs_)
        {
            dp::service::annotation::DeletePvMetadataRequest request;
            request.set_pvnameoralias(source_pv);
            dp::service::annotation::DeletePvMetadataResponse response;
            grpc::ClientContext                               context;
            annotation_stub_->deletePvMetadata(&context, request, &response);
        }
    }

    std::string                                                         nameSpace_;
    std::unique_ptr<dp::service::annotation::DpAnnotationService::Stub> annotation_stub_;
    std::vector<std::string>                                            metadataPvs_;
    std::vector<std::string>                                            configurationNames_;
    std::vector<std::string>                                            activationIds_;
    std::shared_ptr<MLDPPVXSController>                                 controller_;
};

TEST_F(QueryableMldpIntegrationTest, TimeSeriesReturnsAllBidiResponsesWithDenseIntegerUnion)
{
    const auto source_pv = pv("time_series");
    seedTimeSeries(source_pv, 5, 100);

    const auto result = pollSql(
        "SELECT pv, time, value FROM mldp.time_series WHERE pv = " + quote(source_pv) + " AND time >= NOW-300s",
        "time-series rows",
        [](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == 5;
        });

    ASSERT_EQ(rowCount(result), 5);
    // queryDataBidiStream response chunking is owned by MLDP.  The server may
    // return this small result in one response or split it across several;
    // unlike the retired local ts:<offset> pagination, join_batch_size does
    // not dictate backend cursor response boundaries.
    EXPECT_GE(result.stats.rpc_calls, 1u);
    std::vector<int64_t> actual_values;
    std::vector<int64_t> actual_times;
    for (const auto& batch : result.batches)
    {
        EXPECT_EQ(batch->schema()->field(0)->name(), "pv");
        EXPECT_TRUE(batch->schema()->field(1)->type()->Equals(arrow::timestamp(arrow::TimeUnit::NANO, "UTC")));
        ASSERT_EQ(batch->schema()->field(2)->type()->id(), arrow::Type::DENSE_UNION);
        const auto values = std::static_pointer_cast<arrow::DenseUnionArray>(batch->column(2));
        for (int64_t index = 0; index < values->length(); ++index)
        {
            EXPECT_EQ(values->type_code(index), 5);
            const auto child = std::static_pointer_cast<arrow::Int64Array>(values->field(5));
            actual_values.push_back(child->Value(values->value_offset(index)));
            const auto times = std::static_pointer_cast<arrow::TimestampArray>(batch->column(1));
            actual_times.push_back(times->Value(index));
        }
    }
    EXPECT_EQ(actual_values, (std::vector<int64_t>{100, 101, 102, 103, 104}));
    EXPECT_TRUE(std::is_sorted(actual_times.begin(), actual_times.end()));
}

TEST_F(QueryableMldpIntegrationTest, PvStatsReturnsEveryDriverOwnedPage)
{
    std::vector<std::string> pvs;
    for (int index = 0; index < 2; ++index)
    {
        const auto source_pv = pv("stats_" + std::to_string(index));
        seedTimeSeries(source_pv, 1, index);
        pvs.push_back(source_pv);
    }

    const auto result = pollSql(
        "SELECT pv, first_timestamp, last_timestamp, num_buckets FROM mldp.pv_stats WHERE pv IN (" + commaSeparatedQuoted(pvs) + ")",
        "PV statistics",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == static_cast<int64_t>(pvs.size());
        });

    ASSERT_EQ(rowCount(result), static_cast<int64_t>(pvs.size()));
    EXPECT_GT(result.stats.rpc_calls, 1u);
    const auto                            actual_values = strings(result, 0);
    const std::unordered_set<std::string> actual(actual_values.begin(), actual_values.end());
    EXPECT_EQ(actual, std::unordered_set<std::string>(pvs.begin(), pvs.end()));
    for (const auto& batch : result.batches)
    {
        const auto buckets = std::static_pointer_cast<arrow::Int64Array>(batch->column(3));
        for (int64_t index = 0; index < buckets->length(); ++index)
        {
            EXPECT_GE(buckets->Value(index), 1);
        }
    }
}

TEST_F(QueryableMldpIntegrationTest, WideTableUsesPvAndClosedWindowSubqueries)
{
    const std::vector<std::string> source_pvs = {pv("magnet_1"), pv("magnet_2")};
    const auto                     now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    for (std::size_t index = 0; index < source_pvs.size(); ++index)
    {
        seedTimeSeries(source_pvs[index], 3, static_cast<int64_t>(300 + index * 10));
    }
    seedMetadata(source_pvs);
    const auto configuration_name = configName("beam_mode");
    const auto activation_id = configName("beam_mode_activation");
    const auto configuration_category = configName("beam_mode_category");
    seedConfiguration(configuration_name,
                      configuration_category,
                      activation_id,
                      BusTimestamp{.epoch_seconds = now_seconds - 1, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds + 4, .nanoseconds = 0});

    const auto seeded_activation = pollSql(
        "SELECT time, end_time FROM mldp.configuration_activation WHERE activation_id = " + quote(activation_id) +
            " AND end_time IS NOT NULL",
        "closed seeded configuration activation",
        [](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == 1;
        });
    ASSERT_EQ(rowCount(seeded_activation), 1);

    const auto window_sql =
        "SELECT activation.time, activation.end_time " "FROM mldp.configuration_activation activation " "INNER JOIN mldp.configuration configuration ON activation.config_name = configuration.name " "WHERE activation.activation_id = " + quote(activation_id) +
        " AND configuration.name = " + quote(configuration_name) +
        " AND configuration.category = " + quote(configuration_category) + " AND activation.end_time IS NOT NULL";
    const auto windows = pollSql(
        window_sql,
        "closed namespace-filtered configuration activation",
        [](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == 1;
        });
    ASSERT_EQ(rowCount(windows), 1);
    ASSERT_FALSE(windows.batches.empty());
    const auto window_times = std::static_pointer_cast<arrow::TimestampArray>(windows.batches.front()->column(0));
    const auto window_ends = std::static_pointer_cast<arrow::TimestampArray>(windows.batches.front()->column(1));
    ASSERT_EQ(window_times->length(), 1);
    ASSERT_EQ(window_ends->length(), 1);
    EXPECT_EQ(window_times->Value(0) / 1'000'000'000LL, static_cast<int64_t>(now_seconds - 1));
    EXPECT_EQ(window_ends->Value(0) / 1'000'000'000LL, static_cast<int64_t>(now_seconds + 4));

    const auto sql =
        "SELECT * FROM mldp.time_series_table " "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv IN (" + commaSeparatedQuoted(source_pvs) + ")) " "AND window IN (" + window_sql + ")";
    const auto result = pollSql(
        sql,
        "wide table rows",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) > 0;
        });

    ASSERT_GT(rowCount(result), 0);
    ASSERT_FALSE(result.batches.empty());
    const auto& batch = result.batches.front();
    EXPECT_EQ(batch->schema()->field_names(), (std::vector<std::string>{"time", source_pvs[0], source_pvs[1]}));
    const auto times = std::static_pointer_cast<arrow::TimestampArray>(batch->column(0));
    for (int64_t index = 0; index < times->length(); ++index)
    {
        const auto seconds = times->Value(index) / 1'000'000'000LL;
        EXPECT_GE(seconds, static_cast<int64_t>(now_seconds - 1));
        EXPECT_LE(seconds, static_cast<int64_t>(now_seconds + 4));
    }
}

TEST_F(QueryableMldpIntegrationTest, ControllerGeneratedProductionShapedWideWindowQuery)
{
    constexpr int kPvCount = 32;
    constexpr int kSampleCount = 5;
    configureQueryables(4);
    startController();

    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto               metadata_prefix = "USEG:UNDH:" + nameSpace_;
    const auto               configuration_name = configName("wide_window_configuration");
    const auto               activation_id = configName("wide_window_activation");
    std::vector<std::string> source_pvs;
    source_pvs.reserve(kPvCount);

    DataBatch frame;
    for (int sample = 0; sample < kSampleCount; ++sample)
    {
        frame.timestamps.push_back(TimestampEntry{
            .epoch_seconds = now_seconds + static_cast<uint64_t>(sample),
            .nanoseconds = static_cast<uint64_t>(sample),
        });
    }
    for (int pv_index = 0; pv_index < kPvCount; ++pv_index)
    {
        const auto source_pv = pv("wide_" + std::to_string(pv_index));
        source_pvs.push_back(source_pv);
        std::vector<int64_t> values;
        values.reserve(kSampleCount);
        for (int sample = 0; sample < kSampleCount; ++sample)
        {
            values.push_back(static_cast<int64_t>(pv_index * 100 + sample));
        }
        frame.columns.push_back(DataColumn{
            .name = source_pv,
            .values = std::move(values),
        });
    }

    IDataBus::EventBatch time_series_batch;
    time_series_batch.reader_name = "controller-wide-reader";
    time_series_batch.payload = TimeSeriesPayload{
        .root_source_name = nameSpace_,
        .frames = {std::move(frame)},
    };
    ASSERT_TRUE(controller_->push(std::move(time_series_batch)));

    SourceMetadataPayload metadata;
    metadata.root_source_name = nameSpace_;
    for (const auto& source_pv : source_pvs)
    {
        metadata.sources.emplace(source_pv, SourceMetadataEntry{
                                                .aliases = std::vector<std::string>{source_pv + ":alias"},
                                                .tags = std::vector<std::string>{nameSpace_, "wide-window"},
                                                .attributes = {{"dname", metadata_prefix + ":" + source_pv}},
                                                .description = "controller wide-window query metadata",
                                                .modified_by = "queryable_mldp_integration_test",
                                            });
        metadataPvs_.push_back(source_pv);
    }
    IDataBus::EventBatch metadata_batch;
    metadata_batch.reader_name = "controller-wide-reader";
    metadata_batch.payload = std::move(metadata);
    ASSERT_TRUE(controller_->push(std::move(metadata_batch)));

    IDataBus::EventBatch configuration_batch;
    configuration_batch.reader_name = "controller-wide-reader";
    configuration_batch.payload = ConfigurationPayload{
        .root_source_name = nameSpace_,
        .configuration_name = configuration_name,
        .category = configName("wide_window_category"),
        .description = "controller wide-window query configuration",
        .attributes = {{"namespace", nameSpace_}},
        .modified_by = "queryable_mldp_integration_test",
    };
    ASSERT_TRUE(controller_->push(std::move(configuration_batch)));

    IDataBus::EventBatch activation_batch;
    activation_batch.reader_name = "controller-wide-reader";
    activation_batch.payload = ConfigurationActivationPayload{
        .client_activation_id = activation_id,
        .configuration_name = configuration_name,
        .start_time = BusTimestamp{.epoch_seconds = now_seconds, .nanoseconds = 0},
        .end_time = BusTimestamp{.epoch_seconds = now_seconds + 5, .nanoseconds = 0},
        .description = "controller wide-window query activation",
        .tags = std::nullopt,
        .attributes = {{"namespace", nameSpace_}},
        .modified_by = "queryable_mldp_integration_test",
    };
    ASSERT_TRUE(controller_->push(std::move(activation_batch)));
    configurationNames_.push_back(configuration_name);
    activationIds_.push_back(activation_id);

    const auto metadata_visible = pollSql(
        "SELECT pv FROM mldp.pv_metadata WHERE attributes.dname PREFIX " + quote(metadata_prefix),
        "controller-generated PV metadata",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == kPvCount;
        });
    ASSERT_EQ(rowCount(metadata_visible), kPvCount);

    const auto activation_visible = pollSql(
        "SELECT activation_id FROM mldp.configuration_activation WHERE activation_id = " + quote(activation_id) +
            " AND end_time IS NOT NULL",
        "controller-generated closed activation",
        [](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == 1;
        });
    ASSERT_EQ(rowCount(activation_visible), 1);

    const auto time_series_visible = pollSql(
        "SELECT pv, time, value FROM mldp.time_series WHERE pv IN (" + commaSeparatedQuoted(source_pvs) +
            ") AND time >= " + std::to_string(now_seconds),
        "controller-generated time-series samples",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == kPvCount * kSampleCount;
        });
    ASSERT_EQ(rowCount(time_series_visible), kPvCount * kSampleCount);

    const auto sql =
        "SELECT * FROM mldp.time_series_table " "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE attributes.dname PREFIX " + quote(metadata_prefix) + ") " "AND window IN (SELECT time, time + 5s FROM mldp.configuration_activation WHERE activation_id = " +
        quote(activation_id) + "; slice 5s, series_per_shard 2);";
    char                           arg0[] = "query";
    char                           arg1[] = "--no-stats";
    char*                          argv[] = {arg0, arg1};
    cli::QuerySubcommand           query_subcommand;
    std::istringstream             input(".format json\n" + sql + "\n.quit\n");
    std::ostringstream             output;
    std::ostringstream             error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.mldp-pool.query-url=dp-query:50052",
        "queryable.mldp.mldp-pool.min-conn=1",
        "queryable.mldp.mldp-pool.max-conn=4",
        "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=dp-annotation:50053",
        "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1",
        "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=4",
    };

    ASSERT_EQ(query_subcommand.run(2, argv, config_sources, input, output, error), 0) << error.str();
    EXPECT_TRUE(error.str().empty()) << error.str();
    const auto repl_output = output.str();
    EXPECT_NE(repl_output.find("Output format: json"), std::string::npos);
    for (const auto& source_pv : source_pvs)
    {
        EXPECT_NE(repl_output.find("\"" + source_pv + "\":"), std::string::npos)
            << "REPL result omitted column '" << source_pv << "'";
    }
    EXPECT_NE(repl_output.find("\"time\":"), std::string::npos);
}

TEST_F(QueryableMldpIntegrationTest, WideTableUsesNormalizedLiteralWindow)
{
    const auto source_pv = pv("literal_window");
    seedTimeSeries(source_pv, 3, 500);

    const auto now_seconds = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto result = pollSql(
        "SELECT * FROM mldp.time_series_table WHERE pv = " + quote(source_pv) +
            " AND window IN (" + std::to_string(now_seconds + 4) + ", " + std::to_string(now_seconds - 1) + ")",
        "wide table rows within literal window",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) > 0;
        });

    ASSERT_GT(rowCount(result), 0);
    ASSERT_FALSE(result.batches.empty());
    const auto& batch = result.batches.front();
    ASSERT_EQ(batch->schema()->field_names(), (std::vector<std::string>{"time", source_pv}));
    const auto times = std::static_pointer_cast<arrow::TimestampArray>(batch->column(0));
    for (int64_t index = 0; index < times->length(); ++index)
    {
        const auto seconds = times->Value(index) / 1'000'000'000LL;
        EXPECT_GE(seconds, now_seconds - 1);
        EXPECT_LE(seconds, now_seconds + 4);
    }
}

TEST_F(QueryableMldpIntegrationTest, ConfigurationActivationsFilterByTagsAndAttributes)
{
    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto configuration_name = configName("metadata_filter_configuration");
    const auto activation_id = configName("metadata_filter_activation");
    const auto activation_tag = configName("metadata_filter_tag");
    const auto attribute_key = "query_filter";
    const auto attribute_value = configName("metadata_filter_value");
    seedConfiguration(configuration_name,
                      "query-integration-category",
                      activation_id,
                      BusTimestamp{.epoch_seconds = now_seconds - 1, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds + 1, .nanoseconds = 0},
                      std::vector<std::string>{activation_tag},
                      {{attribute_key, attribute_value}});

    const auto expect_seeded_activation = [&](const std::string& sql, const std::string& description)
    {
        const auto result = pollSql(
            sql,
            description,
            [&](const QueryExecutionResult& candidate)
            {
                return rowCount(candidate) == 1 && strings(candidate, 0) == std::vector<std::string>{activation_id};
            });
        ASSERT_EQ(rowCount(result), 1);
        EXPECT_EQ(strings(result, 0), std::vector<std::string>{activation_id});
        EXPECT_EQ(result.stats.rpc_calls, 1u);
    };

    expect_seeded_activation(
        "SELECT activation_id FROM mldp.configuration_activation WHERE tag = " + quote(activation_tag),
        "configuration activation selected by tag");
    expect_seeded_activation(
        std::string{"SELECT activation_id FROM mldp.configuration_activation WHERE attributes."} + attribute_key + " = " + quote(attribute_value),
        "configuration activation selected by attribute");
    expect_seeded_activation(
        std::string{"SELECT activation_id FROM mldp.configuration_activation WHERE tag = "} + quote(activation_tag) +
            " AND attributes." + attribute_key + " = " + quote(attribute_value),
        "configuration activation selected by tag and attribute");
}

TEST_F(QueryableMldpIntegrationTest, AnnotationTablesAndJoinsReturnOnlySeededRecords)
{
    const auto                     source_pv = pv("metadata_join");
    const std::vector<std::string> metadata_pvs = {source_pv, pv("metadata_page_1")};
    const std::vector<std::string> configuration_names = {
        configName("configuration_0"), configName("configuration_1")};
    const std::vector<std::string> activation_ids = {
        configName("activation_0"), configName("activation_1")};
    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto active_configuration_name = configName("active_configuration");
    const auto active_activation_id = configName("active_activation");
    seedTimeSeries(source_pv, 3, 200);
    seedMetadata(metadata_pvs);
    for (std::size_t index = 0; index < configuration_names.size(); ++index)
    {
        seedConfiguration(
            configuration_names[index],
            "query-integration-category",
            activation_ids[index],
            BusTimestamp{.epoch_seconds = now_seconds - 3 + index, .nanoseconds = 0},
            BusTimestamp{.epoch_seconds = now_seconds - 2 + index, .nanoseconds = 0});
    }
    seedConfiguration(active_configuration_name,
                      "query-integration-active-category",
                      active_activation_id,
                      BusTimestamp{.epoch_seconds = static_cast<uint64_t>(now_seconds - 1), .nanoseconds = 0});

    const auto metadata = pollSql(
        "SELECT pv, description FROM mldp.pv_metadata WHERE pv IN (" + commaSeparatedQuoted(metadata_pvs) + ")",
        "PV metadata",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == static_cast<int64_t>(metadata_pvs.size());
        });
    ASSERT_EQ(rowCount(metadata), static_cast<int64_t>(metadata_pvs.size()));
    // Annotation query clients consume backend continuation pages internally,
    // so the executor performs one queryable call for this table scan.
    EXPECT_EQ(metadata.stats.rpc_calls, 1u);
    const auto metadata_rows = strings(metadata, 0);
    EXPECT_EQ(std::unordered_set<std::string>(metadata_rows.begin(), metadata_rows.end()),
              std::unordered_set<std::string>(metadata_pvs.begin(), metadata_pvs.end()));

    const auto all_metadata = pollSql(
        "SELECT pv FROM mldp.pv_metadata",
        "all PV metadata",
        [&](const QueryExecutionResult& candidate)
        {
            const auto returned_pvs = strings(candidate, 0);
            return std::all_of(metadata_pvs.begin(), metadata_pvs.end(), [&](const std::string& pv)
                               {
                                   return std::find(returned_pvs.begin(), returned_pvs.end(), pv) != returned_pvs.end();
                               });
        });
    const auto all_metadata_pvs = strings(all_metadata, 0);
    for (const auto& pv : metadata_pvs)
        EXPECT_NE(std::find(all_metadata_pvs.begin(), all_metadata_pvs.end(), pv), all_metadata_pvs.end());

    const auto configuration = pollSql(
        "SELECT name, category FROM mldp.configuration WHERE name IN (" + commaSeparatedQuoted(configuration_names) + ")",
        "configuration",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == static_cast<int64_t>(configuration_names.size());
        });
    ASSERT_EQ(rowCount(configuration), static_cast<int64_t>(configuration_names.size()));
    EXPECT_EQ(configuration.stats.rpc_calls, 1u);

    const auto all_configurations = pollSql(
        "SELECT name FROM mldp.configuration",
        "all configurations",
        [&](const QueryExecutionResult& candidate)
        {
            const auto returned_names = strings(candidate, 0);
            return std::all_of(configuration_names.begin(), configuration_names.end(), [&](const std::string& name)
                               {
                                   return std::find(returned_names.begin(), returned_names.end(), name) != returned_names.end();
                               }) &&
                   std::find(returned_names.begin(), returned_names.end(), active_configuration_name) != returned_names.end();
        });
    const auto all_configuration_names = strings(all_configurations, 0);
    for (const auto& name : configuration_names)
        EXPECT_NE(std::find(all_configuration_names.begin(), all_configuration_names.end(), name), all_configuration_names.end());
    EXPECT_NE(std::find(all_configuration_names.begin(), all_configuration_names.end(), active_configuration_name), all_configuration_names.end());

    const auto activation = pollSql(
        "SELECT config_name, activation_id FROM mldp.configuration_activation WHERE activation_id IN (" + commaSeparatedQuoted(activation_ids) + ")",
        "configuration activation",
        [&](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == static_cast<int64_t>(activation_ids.size());
        });
    ASSERT_EQ(rowCount(activation), static_cast<int64_t>(activation_ids.size()));
    EXPECT_EQ(activation.stats.rpc_calls, 1u);

    const auto active = pollSql(
        "SELECT name, activation_id FROM mldp.active_configurations WHERE at = NOW",
        "active configuration",
        [&](const QueryExecutionResult& candidate)
        {
            const auto returned_activation_ids = strings(candidate, 1);
            return std::find(returned_activation_ids.begin(), returned_activation_ids.end(), active_activation_id) != returned_activation_ids.end();
        });
    const auto active_activation_ids = strings(active, 1);
    EXPECT_NE(std::find(active_activation_ids.begin(), active_activation_ids.end(), active_activation_id), active_activation_ids.end());

    const auto metadata_join = pollSql(
        "SELECT ts.pv, ts.value, meta.description FROM mldp.time_series ts " "INNER JOIN mldp.pv_metadata meta ON ts.pv = meta.pv " "WHERE ts.pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv = " + quote(source_pv) + ") " "AND meta.pv = " + quote(source_pv) + " AND ts.time >= NOW-300s",
        "metadata/time-series join",
        [](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == 3;
        });
    ASSERT_EQ(rowCount(metadata_join), 3);
    for (const auto& joined_pv : strings(metadata_join, 0))
    {
        EXPECT_EQ(joined_pv, source_pv);
    }

    const auto configuration_join = pollSql(
        "SELECT activation.config_name, configuration.category FROM mldp.configuration_activation activation " "INNER JOIN mldp.configuration configuration ON activation.config_name = configuration.name " "WHERE activation.activation_id = " + quote(activation_ids.front()) + " AND configuration.name = " + quote(configuration_names.front()),
        "configuration activation/configuration join",
        [](const QueryExecutionResult& candidate)
        {
            return rowCount(candidate) == 1;
        });
    ASSERT_EQ(rowCount(configuration_join), 1);
    EXPECT_EQ(strings(configuration_join, 0), std::vector<std::string>{configuration_names.front()});
    EXPECT_EQ(strings(configuration_join, 1), std::vector<std::string>{"query-integration-category"});
}

} // namespace
