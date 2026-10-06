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
#include <query/QueryCommand.h>
#include <query/NullQueryCommandListener.h>
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PhysicalPlan.h>
#include <util/bus/IDataBus.h>
#include <writer/WriterFactory.h>

#include <arrow/array.h>
#include <arrow/scalar.h>
#include <arrow/type.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "../config/test_config_helpers.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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
        auto& seeded = seededTimestamps_[source_pv];
        seeded.clear();
        for (int index = 0; index < count; ++index)
        {
            frame.timestamps.push_back(TimestampEntry{
                .epoch_seconds = static_cast<uint64_t>(now_seconds + index),
                .nanoseconds = static_cast<uint64_t>(index)});
            seeded.push_back(frame.timestamps.back());
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

    /** Saves one status per seeded sample of @p source_pv (codes[i] for sample i) in (domain, layer). */
    void seedSampleStatuses(const std::string& source_pv, const std::string& domain, const std::string& layer, const std::vector<int32_t>& codes)
    {
        const auto& timestamps = seededTimestamps_.at(source_pv);
        ASSERT_EQ(timestamps.size(), codes.size());
        {
            dp::service::annotation::SaveSampleStatusDomainRequest request;
            request.set_domainname(domain);
            dp::service::annotation::SaveSampleStatusDomainResponse response;
            grpc::ClientContext                                     context;
            ASSERT_TRUE(annotation_stub_->saveSampleStatusDomain(&context, request, &response).ok());
        }
        dp::service::annotation::SaveSampleStatusesRequest request;
        request.set_modifiedby("queryable_mldp_integration_test");
        auto* frame = request.add_frames();
        frame->set_domain(domain);
        frame->set_layer(layer);
        auto* timestamp_list = frame->mutable_datatimestamps()->mutable_timestamplist();
        for (const auto& timestamp : timestamps)
        {
            auto* entry = timestamp_list->add_timestamps();
            entry->set_epochseconds(timestamp.epoch_seconds);
            entry->set_nanoseconds(timestamp.nanoseconds);
        }
        auto* column = frame->add_statuscolumns();
        column->set_pvname(source_pv);
        for (const auto code : codes) column->add_statuscodes(code);
        dp::service::annotation::SaveSampleStatusesResponse response;
        grpc::ClientContext                                 context;
        const auto                                          status = annotation_stub_->saveSampleStatuses(&context, request, &response);
        ASSERT_TRUE(status.ok()) << status.error_message();
        ASSERT_TRUE(response.has_savesamplestatusesresult()) << response.exceptionalresult().message();
        sampleStatuses_.push_back({source_pv, domain, layer});
    }

    void seedMetadata(const std::vector<std::string>& pvs, const std::vector<std::string>& extra_tags = {})
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
                                                   .tags = [&]()
                                                   {
                                                       std::vector<std::string> tags{nameSpace_, "magnet"};
                                                       tags.insert(tags.end(), extra_tags.begin(), extra_tags.end());
                                                       return tags;
                                                   }(),
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
        for (const auto& [source_pv, domain, layer] : sampleStatuses_)
        {
            dp::service::annotation::DeleteSampleStatusesRequest request;
            request.mutable_timerange()->mutable_begintime()->set_epochseconds(1);
            request.mutable_timerange()->mutable_endtime()->set_epochseconds(253'402'300'799LL);
            request.add_pvnames(source_pv);
            request.set_domain(domain);
            request.set_layer(layer);
            dp::service::annotation::DeleteSampleStatusesResponse response;
            grpc::ClientContext                                   context;
            annotation_stub_->deleteSampleStatuses(&context, request, &response);
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
    std::unordered_map<std::string, std::vector<TimestampEntry>>        seededTimestamps_;
    std::vector<std::tuple<std::string, std::string, std::string>>      sampleStatuses_;
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
    EXPECT_GE(result.stats.rpc_calls, 1u);
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
    cli::NullQueryCommandListener query_listener;
    cli::QueryCommand             query_subcommand(query_listener);
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
    // An explicit select list that does not project the attributes map streams one
    // batch per backend continuation page, so the executor sees one call per page.
    EXPECT_EQ(metadata.stats.rpc_calls, static_cast<uint64_t>(metadata_pvs.size()));
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
    EXPECT_EQ(configuration.stats.rpc_calls, static_cast<uint64_t>(configuration_names.size()));

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
    EXPECT_EQ(activation.stats.rpc_calls, static_cast<uint64_t>(activation_ids.size()));

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

TEST_F(QueryableMldpIntegrationTest, SelectDistinctDeduplicatesTimeSeriesAndAnnotationRows)
{
    const auto                     source_pv = pv("distinct_series");
    const std::vector<std::string> metadata_pvs = {pv("distinct_meta_0"), pv("distinct_meta_1"), pv("distinct_meta_2")};
    const std::vector<std::string> configuration_names = {configName("distinct_cfg_0"), configName("distinct_cfg_1")};
    const auto                     now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    seedTimeSeries(source_pv, 5, 300);
    // Every seeded metadata record shares the same description.
    seedMetadata(metadata_pvs);
    for (std::size_t index = 0; index < configuration_names.size(); ++index)
    {
        seedConfiguration(configuration_names[index],
                          "query-integration-distinct-category",
                          configName("distinct_activation_" + std::to_string(index)),
                          BusTimestamp{.epoch_seconds = now_seconds - 3 + index, .nanoseconds = 0},
                          BusTimestamp{.epoch_seconds = now_seconds - 2 + index, .nanoseconds = 0});
    }

    // Time series: five samples of one PV collapse to one distinct pv, but stay five distinct (pv, value) rows.
    const auto series_filter = " FROM mldp.time_series WHERE pv = " + quote(source_pv) + " AND time >= NOW-300s";
    pollSql("SELECT pv, value" + series_filter, "time-series rows", [](const QueryExecutionResult& candidate)
            { return rowCount(candidate) == 5; });
    const auto distinct_pv = executeSql("SELECT DISTINCT pv" + series_filter);
    ASSERT_EQ(rowCount(distinct_pv), 1);
    EXPECT_EQ(strings(distinct_pv, 0), std::vector<std::string>{source_pv});
    EXPECT_EQ(rowCount(executeSql("SELECT DISTINCT pv, value" + series_filter)), 5);

    // Annotation tables: shared description and category collapse to a single row.
    const auto metadata_filter = " FROM mldp.pv_metadata WHERE pv IN (" + commaSeparatedQuoted(metadata_pvs) + ")";
    pollSql("SELECT pv" + metadata_filter, "PV metadata", [&](const QueryExecutionResult& candidate)
            { return rowCount(candidate) == static_cast<int64_t>(metadata_pvs.size()); });
    const auto distinct_description = executeSql("SELECT DISTINCT description" + metadata_filter);
    ASSERT_EQ(rowCount(distinct_description), 1);
    EXPECT_EQ(strings(distinct_description, 0), std::vector<std::string>{"query integration metadata"});
    EXPECT_EQ(rowCount(executeSql("SELECT DISTINCT pv" + metadata_filter)), static_cast<int64_t>(metadata_pvs.size()));

    const auto configuration_filter = " FROM mldp.configuration WHERE name IN (" + commaSeparatedQuoted(configuration_names) + ")";
    pollSql("SELECT name" + configuration_filter, "configuration", [&](const QueryExecutionResult& candidate)
            { return rowCount(candidate) == static_cast<int64_t>(configuration_names.size()); });
    const auto distinct_category = executeSql("SELECT DISTINCT category" + configuration_filter);
    ASSERT_EQ(rowCount(distinct_category), 1);
    EXPECT_EQ(strings(distinct_category, 0), std::vector<std::string>{"query-integration-distinct-category"});

    // LIMIT counts distinct rows and is not pushed to the backend page size.
    const auto limited_sql = "SELECT DISTINCT description" + metadata_filter + " LIMIT 1";
    EXPECT_EQ(plan::physicalPlanToString(QueryPlanner{}.plan(parseQuery(limited_sql))).find("row_limit="), std::string::npos);
    const auto limited = executeSql(limited_sql);
    EXPECT_EQ(rowCount(limited), 1);
}

TEST_F(QueryableMldpIntegrationTest, SelectDistinctDeduplicatesActivationsAndWideTableRows)
{
    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    // Activations: two activations of the same configuration collapse to one distinct config_name.
    const auto                     configuration_name = configName("distinct_activation_cfg");
    const std::vector<std::string> activation_ids = {configName("distinct_act_0"), configName("distinct_act_1")};
    for (std::size_t index = 0; index < activation_ids.size(); ++index)
    {
        seedConfiguration(configuration_name,
                          "query-integration-distinct-category",
                          activation_ids[index],
                          BusTimestamp{.epoch_seconds = now_seconds - 4 + 2 * index, .nanoseconds = 0},
                          BusTimestamp{.epoch_seconds = now_seconds - 3 + 2 * index, .nanoseconds = 0});
    }
    const auto activation_filter = " FROM mldp.configuration_activation WHERE activation_id IN (" + commaSeparatedQuoted(activation_ids) + ")";
    pollSql("SELECT activation_id" + activation_filter, "configuration activations", [&](const QueryExecutionResult& candidate)
            { return rowCount(candidate) == static_cast<int64_t>(activation_ids.size()); });
    const auto distinct_config = executeSql("SELECT DISTINCT config_name" + activation_filter);
    ASSERT_EQ(rowCount(distinct_config), 1);
    EXPECT_EQ(strings(distinct_config, 0), std::vector<std::string>{configuration_name});
    EXPECT_EQ(rowCount(executeSql("SELECT DISTINCT activation_id" + activation_filter)), static_cast<int64_t>(activation_ids.size()));

    // Wide table: only SELECT * is allowed, so DISTINCT * runs over the pivoted rows. Each row has
    // its own time, so de-duplication must keep every row and the PV column layout.
    const auto source_pv = pv("distinct_wide");
    seedTimeSeries(source_pv, 3, 700);
    const auto wide_filter = " FROM mldp.time_series_table WHERE pv = " + quote(source_pv) +
                             " AND window IN (" + std::to_string(now_seconds - 1) + ", " + std::to_string(now_seconds + 4) + ")";
    const auto wide_all = pollSql("SELECT *" + wide_filter, "wide table rows", [](const QueryExecutionResult& candidate)
                                  { return rowCount(candidate) == 3; });
    ASSERT_EQ(rowCount(wide_all), 3);
    const auto wide_distinct = executeSql("SELECT DISTINCT *" + wide_filter);
    ASSERT_EQ(rowCount(wide_distinct), 3);
    ASSERT_FALSE(wide_distinct.batches.empty());
    EXPECT_EQ(wide_distinct.batches.front()->schema()->field_names(), (std::vector<std::string>{"time", source_pv}));
    const auto wide_plan = plan::physicalPlanToString(QueryPlanner{}.plan(parseQuery("SELECT DISTINCT *" + wide_filter)));
    EXPECT_NE(wide_plan.find("distinct=true"), std::string::npos);
    EXPECT_NE(wide_plan.find("PhysicalPivot"), std::string::npos);
}

} // namespace

TEST_F(QueryableMldpIntegrationTest, PvNamePatternSelectsMatchingSeriesWithoutPvList)
{
    seedTimeSeries(pv("pattern_a"), 2, 10);
    seedTimeSeries(pv("pattern_b"), 2, 20);
    seedTimeSeries(pv("other"), 2, 30);

    // The namespace is unique, so these patterns match only this test's PVs.
    const auto window = " AND time >= NOW-300s";
    const auto prefix = pollSql("SELECT DISTINCT pv FROM mldp.time_series WHERE pv PREFIX " + quote(pv("pattern_")) + window, "prefix-matched series",
                                [](const QueryExecutionResult& candidate) { return rowCount(candidate) == 2; });
    auto names = strings(prefix, 0);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{pv("pattern_a"), pv("pattern_b")}));

    // LIKE is case-insensitive and anchored at both ends.
    std::string upper = nameSpace_ + ":PATTERN_%";
    std::transform(upper.begin(), upper.end(), upper.begin(), [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    EXPECT_EQ(rowCount(executeSql("SELECT DISTINCT pv FROM mldp.time_series WHERE pv LIKE " + quote(upper) + window)), 2);
    EXPECT_EQ(rowCount(executeSql("SELECT DISTINCT pv FROM mldp.time_series WHERE pv CONTAINS " + quote(nameSpace_ + ":oth") + window)), 1);

    // pv_stats accepts the same pattern pushdown.
    EXPECT_EQ(rowCount(executeSql("SELECT pv FROM mldp.pv_stats WHERE pv PREFIX " + quote(pv("pattern_")))), 2);

    // The pattern is sent to MLDP: no explicit PV list is required any more.
    const auto plan_text = plan::physicalPlanToString(QueryPlanner{}.plan(parseQuery("SELECT pv FROM mldp.time_series WHERE pv PREFIX 'x'")));
    EXPECT_NE(plan_text.find("PhysicalTableScan(table=mldp.time_series)"), std::string::npos);
}

TEST_F(QueryableMldpIntegrationTest, PvMetadataTagSelectsSeriesOnTheBackend)
{
    const auto tagged = pv("meta_tagged");
    const auto untagged = pv("meta_untagged");
    const auto tag = nameSpace_ + "_selected";
    seedTimeSeries(tagged, 3, 40);
    seedTimeSeries(untagged, 3, 50);
    seedMetadata({tagged}, {tag});
    seedMetadata({untagged});

    const auto sql = "SELECT pv, value FROM mldp.time_series WHERE pv_tag = " + quote(tag) + " AND time >= NOW-300s";
    const auto result = pollSql(sql, "tag-selected series", [](const QueryExecutionResult& candidate) { return rowCount(candidate) == 3; });
    for (const auto& name : strings(result, 0)) EXPECT_EQ(name, tagged);

    // The PV-name pattern narrows the metadata selection further.
    EXPECT_EQ(rowCount(executeSql("SELECT pv FROM mldp.time_series WHERE pv_tag = " + quote(tag) + " AND pv PREFIX " + quote(pv("nothing")) +
                                  " AND time >= NOW-300s")),
              0);
}

TEST_F(QueryableMldpIntegrationTest, ConfigurationSelectorRestrictsSamplesToActivationIntervals)
{
    const auto source_pv = pv("config_series");
    seedTimeSeries(source_pv, 6, 600);
    const auto& timestamps = seededTimestamps_.at(source_pv);
    // Activation covers samples 1..3 (inclusive start, exclusive end at sample 4).
    const auto config = configName("selector_cfg");
    seedConfiguration(config, configName("selector_category"), configName("selector_activation"),
                      BusTimestamp{.epoch_seconds = timestamps[1].epoch_seconds, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = timestamps[4].epoch_seconds, .nanoseconds = 0});

    pollSql("SELECT activation_id FROM mldp.configuration_activation WHERE config_name = " + quote(config), "selector activation",
            [](const QueryExecutionResult& candidate) { return rowCount(candidate) == 1; });
    const auto base = "SELECT pv, value FROM mldp.time_series WHERE pv = " + quote(source_pv) + " AND time >= NOW-300s";
    pollSql(base, "selector series", [](const QueryExecutionResult& candidate) { return rowCount(candidate) == 6; });

    const auto restricted = executeSql(base + " AND config_name = " + quote(config));
    // Exactly the samples inside the activation (1, 2, 3), on both tables.
    EXPECT_EQ(rowCount(restricted), 3);
    EXPECT_EQ(rowCount(executeSql("SELECT * FROM mldp.time_series_table WHERE pv = " + quote(source_pv) + " AND time >= NOW-300s AND config_name = " + quote(config))), 3);
    EXPECT_EQ(rowCount(executeSql(base + " AND config_name = " + quote(configName("no_such_cfg")))), 0);
}

TEST_F(QueryableMldpIntegrationTest, UnfilteredActivationScanReturnsEveryActivation)
{
    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto first = configName("browse_activation_0");
    const auto second = configName("browse_activation_1");
    // The service rejects activations overlapping one of the same configuration
    // or category, so the seeds use closed, disjoint windows.
    seedConfiguration(configName("browse_cfg_0"), configName("browse_category_0"), first, BusTimestamp{.epoch_seconds = now_seconds - 10, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds - 8, .nanoseconds = 0});
    seedConfiguration(configName("browse_cfg_1"), configName("browse_category_1"), second, BusTimestamp{.epoch_seconds = now_seconds - 6, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds - 4, .nanoseconds = 0});

    // No predicate at all used to be rejected by the driver.
    const auto result = pollSql("SELECT activation_id FROM mldp.configuration_activation", "unfiltered activations",
                                [&](const QueryExecutionResult& candidate)
                                {
                                    const auto ids = strings(candidate, 0);
                                    return std::count(ids.begin(), ids.end(), first) == 1 && std::count(ids.begin(), ids.end(), second) == 1;
                                });
    EXPECT_GE(rowCount(result), 2);
}

TEST_F(QueryableMldpIntegrationTest, DistinctOnKeepsOneRowPerKeyChosenByOrderBy)
{
    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto config = configName("distinct_on_cfg");
    // Two activations of one configuration, plus one of another.
    seedConfiguration(config, configName("distinct_on_category"), configName("distinct_on_old"), BusTimestamp{.epoch_seconds = now_seconds - 20, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds - 15, .nanoseconds = 0});
    seedConfiguration(config, configName("distinct_on_category"), configName("distinct_on_new"), BusTimestamp{.epoch_seconds = now_seconds - 10, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds - 5, .nanoseconds = 0});
    seedConfiguration(configName("distinct_on_other"), configName("distinct_on_other_category"), configName("distinct_on_single"),
                      BusTimestamp{.epoch_seconds = now_seconds - 8, .nanoseconds = 0}, BusTimestamp{.epoch_seconds = now_seconds - 7, .nanoseconds = 0});

    const auto filter = " FROM mldp.configuration_activation WHERE config_name IN (" + quote(config) + ", " + quote(configName("distinct_on_other")) + ")";
    pollSql("SELECT activation_id" + filter, "distinct-on activations", [](const QueryExecutionResult& candidate) { return rowCount(candidate) == 3; });

    const auto latest = executeSql("SELECT DISTINCT ON (config_name) config_name, activation_id" + filter + " ORDER BY time DESC");
    ASSERT_EQ(rowCount(latest), 2);
    std::map<std::string, std::string> by_config;
    for (const auto& batch : latest.batches)
        for (int64_t row = 0; row < batch->num_rows(); ++row)
            by_config[(*batch->column(0)->GetScalar(row))->ToString()] = (*batch->column(1)->GetScalar(row))->ToString();
    EXPECT_EQ(by_config[config], configName("distinct_on_new"));

    const auto earliest = executeSql("SELECT DISTINCT ON (config_name) config_name, activation_id" + filter + " ORDER BY time ASC");
    std::set<std::string> earliest_ids;
    for (const auto& id : strings(earliest, 1)) earliest_ids.insert(id);
    EXPECT_TRUE(earliest_ids.contains(configName("distinct_on_old")));
    // Plain DISTINCT on the same columns keeps both activations of the repeated configuration.
    EXPECT_EQ(rowCount(executeSql("SELECT DISTINCT config_name, activation_id" + filter)), 3);
}

TEST_F(QueryableMldpIntegrationTest, GroupByAggregatesSeededSeriesExactly)
{
    const auto first = pv("group_a");
    const auto second = pv("group_b");
    seedTimeSeries(first, 4, 10);  // 10, 11, 12, 13
    seedTimeSeries(second, 2, 100); // 100, 101

    const auto filter = " FROM mldp.time_series WHERE pv IN (" + quote(first) + ", " + quote(second) + ") AND time >= NOW-300s";
    pollSql("SELECT pv" + filter, "grouped series", [](const QueryExecutionResult& candidate) { return rowCount(candidate) == 6; });

    const auto grouped = executeSql("SELECT pv, COUNT(*) AS n, SUM(value), AVG(value), MIN(value), MAX(value), FIRST(value), LAST(value)" + filter +
                                    " GROUP BY pv ORDER BY pv");
    ASSERT_EQ(rowCount(grouped), 2);
    const auto& batch = grouped.batches.front();
    const auto cell = [&batch](const int column, const int64_t row) { return (*batch->column(column)->GetScalar(row))->ToString(); };
    EXPECT_EQ(batch->schema()->field(1)->name(), "n");
    EXPECT_EQ(cell(0, 0), first);
    EXPECT_EQ(cell(1, 0), "4");
    EXPECT_EQ(cell(2, 0), "46");
    EXPECT_EQ(cell(3, 0), "11.5");
    EXPECT_EQ(cell(4, 0), "10");
    EXPECT_EQ(cell(5, 0), "13");
    EXPECT_EQ(cell(6, 0), "10");
    EXPECT_EQ(cell(7, 0), "13");
    EXPECT_EQ(cell(0, 1), second);
    EXPECT_EQ(cell(1, 1), "2");

    const auto having = executeSql("SELECT pv, COUNT(*)" + filter + " GROUP BY pv HAVING COUNT(*) > 2");
    ASSERT_EQ(rowCount(having), 1);
    EXPECT_EQ(strings(having, 0).front(), first);

    const auto global = executeSql("SELECT COUNT(*), COUNT(DISTINCT pv), MAX(value)" + filter);
    ASSERT_EQ(rowCount(global), 1);
    EXPECT_EQ((*global.batches.front()->column(0)->GetScalar(0))->ToString(), "6");
    EXPECT_EQ((*global.batches.front()->column(1)->GetScalar(0))->ToString(), "2");
    EXPECT_EQ((*global.batches.front()->column(2)->GetScalar(0))->ToString(), "101");

    // Grouping annotation records works the same way.
    const auto now_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto config = configName("group_cfg");
    seedConfiguration(config, configName("group_category"), configName("group_activation_0"), BusTimestamp{.epoch_seconds = now_seconds - 9, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds - 8, .nanoseconds = 0});
    seedConfiguration(config, configName("group_category"), configName("group_activation_1"), BusTimestamp{.epoch_seconds = now_seconds - 6, .nanoseconds = 0},
                      BusTimestamp{.epoch_seconds = now_seconds - 5, .nanoseconds = 0});
    const auto activations = pollSql("SELECT config_name, COUNT(*) FROM mldp.configuration_activation WHERE config_name = " + quote(config) + " GROUP BY config_name",
                                     "grouped activations",
                                     [](const QueryExecutionResult& candidate)
                                     {
                                         return rowCount(candidate) == 1 && (*candidate.batches.front()->column(1)->GetScalar(0))->ToString() == "2";
                                     });
    EXPECT_EQ(rowCount(activations), 1);
}

TEST_F(QueryableMldpIntegrationTest, SampleStatusSelectorIncludesOrExcludesLabeledSamples)
{
    const auto source_pv = pv("status_series");
    seedTimeSeries(source_pv, 4, 800); // 800, 801, 802, 803
    const auto domain = nameSpace_ + "_quality";
    // Codes per sample: 0 good, 2 bad, 0 good, 3 suspect.
    seedSampleStatuses(source_pv, domain, "auto", {0, 2, 0, 3});

    const auto base = "SELECT * FROM mldp.time_series_table WHERE pv = " + quote(source_pv) + " AND time >= NOW-300s AND status_domain = " + quote(domain);
    const auto values = [](const QueryExecutionResult& result)
    {
        std::vector<std::string> output;
        for (const auto& batch : result.batches)
            for (int64_t row = 0; row < batch->num_rows(); ++row)
                if (batch->column(1)->IsValid(row)) output.push_back((*batch->column(1)->GetScalar(row))->ToString());
        std::sort(output.begin(), output.end());
        return output;
    };

    const auto bad = pollSql(base + " AND status_code IN (2, 3)", "status-included samples",
                             [&](const QueryExecutionResult& candidate) { return values(candidate).size() == 2; });
    EXPECT_EQ(values(bad), (std::vector<std::string>{"801", "803"}));

    const auto good = executeSql(base + " AND status_code IN (2, 3) AND status_mode = 'exclude'");
    EXPECT_EQ(values(good), (std::vector<std::string>{"800", "802"}));

    // The long table runs status filters on the same per-sample query.
    EXPECT_EQ(rowCount(executeSql("SELECT pv, value FROM mldp.time_series WHERE pv = " + quote(source_pv) +
                                  " AND time >= NOW-300s AND status_domain = " + quote(domain) + " AND status_code = 2")),
              1);
}
