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
#include <query/ExecutionContext.h>
#include <query/QueryExecutor.h>
#include <query/QueryPlanner.h>
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
#include <unordered_set>
#include <utility>
#include <vector>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::writer;

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

config::Config makeQueryConfig()
{
    return config::makeConfigFromYaml(
        "provider-name: queryable-mldp-integration\n" "provider-description: real MLDP query integration test\n" "ingestion-url: dp-ingestion:50051\n" "query-url: dp-query:50052\n" "annotation-url: dp-annotation:50053\n" "min-conn: 1\n" "max-conn: 1\n");
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
        cleanupAnnotations();
        QueryableFactory::instance().reset();
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
                                                   .tags = std::vector<std::string>{nameSpace_},
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

    void seedConfiguration(const std::string&          name,
                           const std::string&          category,
                           const std::string&          activation_id,
                           const BusTimestamp&         start_time,
                           std::optional<BusTimestamp> end_time = std::nullopt)
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
            .modified_by = "queryable_mldp_integration_test",
        };
        ASSERT_TRUE(writer->push(std::move(configuration_batch)));

        IDataBus::EventBatch activation_batch;
        activation_batch.payload = ConfigurationActivationPayload{
            .client_activation_id = activation_id,
            .configuration_name = name,
            .start_time = start_time,
            .end_time = end_time,
            .description = "query integration activation",
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
};

TEST_F(QueryableMldpIntegrationTest, TimeSeriesReturnsEveryPageWithDenseIntegerUnion)
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
    EXPECT_GT(result.stats.rpc_calls, 1u);
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
                               { return std::find(returned_pvs.begin(), returned_pvs.end(), pv) != returned_pvs.end(); });
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
                               { return std::find(returned_names.begin(), returned_names.end(), name) != returned_names.end(); }) &&
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
        "SELECT ts.pv, ts.value, meta.description FROM mldp.time_series ts " "INNER JOIN mldp.pv_metadata meta ON ts.pv = meta.pv WHERE ts.pv = " + quote(source_pv) + " AND meta.pv = " + quote(source_pv) + " AND ts.time >= NOW-300s",
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
