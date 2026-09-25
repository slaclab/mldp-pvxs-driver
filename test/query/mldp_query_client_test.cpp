//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/QueryFormatter.h>
#include <query/QueryProgress.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include <arrow/compute/api.h>
#include <arrow/table.h>

#include "../config/test_config_helpers.h"

#include <gtest/gtest.h>

#include <arrow/array.h>
#include <arrow/type.h>
#include <arrow/util/key_value_metadata.h>

#include <atomic>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <query.grpc.pb.h>

#include <algorithm>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;

namespace {

// Helper: drain executeStream into a single concatenated RecordBatch (may be nullptr for empty results).
std::shared_ptr<arrow::RecordBatch> executeAll(MLDPQueryClient& client,
                                               std::string_view table_name,
                                               const std::vector<Predicate>& predicates,
                                               const std::set<std::string>& projection_hint,
                                               const ExecutionContext& context)
{
    auto stream = client.executeStream(table_name, predicates, projection_hint, context);
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    while (auto batch = stream->next())
        batches.push_back(std::move(batch));
    if (batches.empty())
        return nullptr;
    if (batches.size() == 1)
        return batches.front();
    auto table_result = arrow::Table::FromRecordBatches(batches);
    if (!table_result.ok())
        throw std::runtime_error("executeAll: concat failed: " + table_result.status().ToString());
    auto combined = (*table_result)->CombineChunks();
    if (!combined.ok())
        throw std::runtime_error("executeAll: combine chunks failed: " + combined.status().ToString());
    auto batch_result = (*combined)->CombineChunksToBatch();
    if (!batch_result.ok())
        throw std::runtime_error("executeAll: combine chunks to batch failed: " + batch_result.status().ToString());
    return *batch_result;
}



mldp_pvxs_driver::config::Config makeQueryConfig(const std::string& query_url)
{
    return mldp_pvxs_driver::config::makeConfigFromYaml(
        "provider-name: test-provider\n" "ingestion-url: localhost:19999\n" "query-url: " + query_url + "\n" "min-conn: 1\n" "max-conn: 1\n");
}

void addAttribute(dp::service::common::DataColumn* column, const std::string& name, const std::string& value)
{
    auto* attribute = column->mutable_metadata()->add_attributes();
    attribute->set_name(name);
    attribute->set_value(value);
}

class QueryService final : public dp::service::query::DpQueryService::Service
{
public:
    dp::service::query::QuerySamplesRequest last_request;
    dp::service::query::QueryBucketsRequest last_bidi_request;
    bool                                  duplicate_column{false};
    bool                                  oversized_column{false};
    bool                                  scalar_columns{false};
    bool                                  all_null_column{false};
    bool                                  block_query_table{false};
    bool                                  query_started{false};
    bool                                  client_cancelled{false};
    mutable std::mutex                    mutex;
    std::condition_variable               condition;

    // Builds the same fixed dataset consumed by both queryTable (wide, padded
    // with nulls against a shared 2-entry timestamp axis) and
    // queryDataBidiStream (long, one bucket per PV with its own timestamps).
    std::vector<dp::service::common::DataColumn> buildColumns() const
    {
        std::vector<dp::service::common::DataColumn> columns;

        dp::service::common::DataColumn first;
        first.set_name("RF:ONE");
        first.mutable_metadata()->add_tags("rf");
        first.mutable_metadata()->add_tags("sample");
        first.mutable_metadata()->add_tags("mldp_sample");
        first.mutable_metadata()->mutable_provenance()->set_source("ioc-a");
        first.mutable_metadata()->mutable_provenance()->set_process("deterministic-sine");
        addAttribute(&first, "device_group", "RF");
        addAttribute(&first, "ordinal", "1");
        addAttribute(&first, "namespace", "mldp_sample");
        addAttribute(&first, "sample_period_seconds", "1");
        first.add_datavalues()->set_stringvalue("one");
        columns.push_back(first);

        dp::service::common::DataColumn second;
        second.set_name("MAG:ONE");
        second.mutable_metadata()->add_tags("magnet");
        second.mutable_metadata()->mutable_provenance()->set_process("archiver");
        addAttribute(&second, "device_group", "MAGNET");
        addAttribute(&second, "location", "LTU");
        second.add_datavalues()->set_doublevalue(2.5);
        columns.push_back(second);

        if (duplicate_column)
        {
            // querySamples merges same-named columns across pages by accumulating
            // datavalues, so a duplicate column only surfaces as an error once the
            // merged value count exceeds the shared timestamp axis.
            dp::service::common::DataColumn duplicate;
            duplicate.set_name("RF:ONE");
            duplicate.add_datavalues()->set_stringvalue("duplicate-1");
            duplicate.add_datavalues()->set_stringvalue("duplicate-2");
            columns.push_back(duplicate);
        }
        if (oversized_column)
        {
            dp::service::common::DataColumn oversized;
            oversized.set_name("TOO:LONG");
            oversized.add_datavalues()->set_intvalue(1);
            oversized.add_datavalues()->set_intvalue(2);
            oversized.add_datavalues()->set_intvalue(3);
            columns.push_back(oversized);
        }
        if (scalar_columns)
        {
            const auto add_column = [&columns](const std::string& name, const auto& set_value)
            {
                dp::service::common::DataColumn column;
                column.set_name(name);
                set_value(column.add_datavalues());
                columns.push_back(column);
            };
            add_column("BOOL", [](auto* value) { value->set_booleanvalue(true); });
            add_column("U32", [](auto* value) { value->set_uintvalue(3); });
            add_column("U64", [](auto* value) { value->set_ulongvalue(4); });
            add_column("I32", [](auto* value) { value->set_intvalue(-5); });
            add_column("I64", [](auto* value) { value->set_longvalue(-6); });
            add_column("FLOAT", [](auto* value) { value->set_floatvalue(1.25F); });
            add_column("BINARY", [](auto* value) { value->set_bytearrayvalue("bytes"); });
            add_column("TIMESTAMP", [](auto* value) { value->mutable_timestampvalue()->set_epochseconds(7); });
        }
        if (all_null_column)
        {
            dp::service::common::DataColumn column;
            column.set_name("ALL:NULL");
            column.add_datavalues();
            columns.push_back(column);
        }
        return columns;
    }

    // Single-page querySamples response: the wide, null-padded shared axis.
    grpc::Status querySamples(grpc::ServerContext* context,
                              const dp::service::query::QuerySamplesRequest* request,
                              dp::service::query::QuerySamplesResponse* response) override
    {
        {
            const std::lock_guard lock(mutex);
            last_request = *request;
            query_started = true;
        }
        condition.notify_all();
        if (block_query_table)
        {
            while (!context->IsCancelled()) std::this_thread::sleep_for(std::chrono::milliseconds{5});
            {
                const std::lock_guard lock(mutex);
                client_cancelled = true;
            }
            condition.notify_all();
            return grpc::Status(grpc::StatusCode::CANCELLED, "client cancelled");
        }
        auto* table = response->mutable_samplequeryresult()->mutable_columntable();
        auto* timestamps = table->mutable_timestamplist();
        timestamps->add_timestamps()->set_epochseconds(1);
        timestamps->add_timestamps()->set_epochseconds(2);
        for (auto& column : buildColumns())
            *table->add_datacolumns() = std::move(column);
        return grpc::Status::OK;
    }

    // Mirrors querySamples' fixed dataset over the paged queryBuckets RPC:
    // one DataBucket per requested PV that exists in the dataset, each with
    // its own per-value timestamp list (epochs 1, 2, 0, 0, ... by index)
    // instead of querySamples' shared null-padded axis.
    grpc::Status queryBuckets(grpc::ServerContext* /*context*/,
                              const dp::service::query::QueryBucketsRequest* request,
                              dp::service::query::QueryBucketsResponse* response) override
    {
        {
            const std::lock_guard lock(mutex);
            last_bidi_request = *request;
        }
        condition.notify_all();

        const auto columns = buildColumns();
        auto*      result = response->mutable_bucketqueryresult();
        for (const auto& pv_name : request->queryspec().pvselector().pvnamelist().pvnames())
        {
            const auto found = std::find_if(columns.begin(), columns.end(), [&](const dp::service::common::DataColumn& column)
                                            {
                                                return column.name() == pv_name;
                                            });
            if (found == columns.end())
                continue;
            auto* bucket = result->add_databuckets();
            bucket->set_pvname(found->name());
            auto* timestamp_list = bucket->mutable_datatimestamps()->mutable_timestamplist();
            for (int index = 0; index < found->datavalues_size(); ++index)
                timestamp_list->add_timestamps()->set_epochseconds(index == 0 ? 1 : index == 1 ? 2 : 0);
            *bucket->mutable_datavalues()->mutable_datacolumn() = *found;
        }
        return grpc::Status::OK;
    }
};

class BidiQueryService final : public dp::service::query::DpQueryService::Service
{
public:
    std::vector<dp::service::query::QueryBucketsRequest> requests;
    grpc::Status terminal_status{grpc::Status::OK};
    bool release_response{false};
    bool initial_request_received{false};
    bool client_cancelled{false};
    std::function<void()> on_cursor_next;
    std::mutex mutex;
    std::condition_variable condition;

    // Paged unary queryBuckets: first call (empty pageToken) blocks until
    // release_response (or cancellation), then returns one page carrying a
    // nextPageToken; the second call (driven by the client's paging loop)
    // either returns terminal_status (failure propagation test) or a final
    // empty page.
    grpc::Status queryBuckets(grpc::ServerContext* context,
                              const dp::service::query::QueryBucketsRequest* request,
                              dp::service::query::QueryBucketsResponse* response) override
    {
        const bool is_first_page = request->executionoptions().pagetoken().empty();
        {
            const std::lock_guard lock(mutex);
            requests.push_back(*request);
            if (is_first_page)
                initial_request_received = true;
        }
        condition.notify_all();

        if (is_first_page)
        {
            {
                std::unique_lock lock(mutex);
                while (!release_response && !context->IsCancelled())
                {
                    condition.wait_for(lock, std::chrono::milliseconds{10});
                }
            }
            if (context->IsCancelled())
            {
                {
                    const std::lock_guard lock(mutex);
                    client_cancelled = true;
                }
                condition.notify_all();
                return grpc::Status(grpc::StatusCode::CANCELLED, "client cancelled");
            }

            auto* result = response->mutable_bucketqueryresult();
            auto* bucket = result->add_databuckets();
            bucket->set_pvname("TEST:PV");
            bucket->mutable_datatimestamps()->mutable_timestamplist()->add_timestamps()->set_epochseconds(1);
            bucket->mutable_datavalues()->mutable_datacolumn()->add_datavalues()->set_intvalue(42);
            result->set_nextpagetoken("page2");
            return grpc::Status::OK;
        }

        if (on_cursor_next) on_cursor_next();
        condition.notify_all();
        if (!terminal_status.ok())
            return terminal_status;
        response->mutable_bucketqueryresult();
        return grpc::Status::OK;
    }
};

class ObservingStreambuf final : public std::stringbuf
{
public:
    std::atomic<bool> wrote{false};

protected:
    std::streamsize xsputn(const char* data, const std::streamsize count) override
    {
        wrote.store(count > 0, std::memory_order_release);
        return std::stringbuf::xsputn(data, count);
    }

    int overflow(const int character) override
    {
        wrote.store(true, std::memory_order_release);
        return std::stringbuf::overflow(character);
    }
};

} // namespace

TEST(MLDPQueryClientTest, BidiStreamSendsInitialQuerySpecThenCursorNextAndPropagatesFinishFailure)
{
    BidiQueryService service;
    service.terminal_status = grpc::Status(grpc::StatusCode::INTERNAL, "terminal failure");
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool()};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("TEST:PV")}},
        {.column = "time", .op = PredicateOp::GTE, .values = {int64_t{10}}},
        {.column = "time", .op = PredicateOp::LTE, .values = {int64_t{20}}},
    };
    // executeStream() now eagerly drains the bidi cursor to completion before
    // returning, so the server-side driving (release the buffered response,
    // then let the failed cursor-next Finish propagate) must happen on a
    // separate thread while the main thread is blocked inside the call.
    std::thread driver([&]
                        {
                            std::unique_lock lock(service.mutex);
                            ASSERT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.initial_request_received; }));
                            ASSERT_EQ(service.requests.size(), 1U);
                            EXPECT_TRUE(service.requests.front().has_queryspec());
                            EXPECT_EQ(service.requests.front().queryspec().timerange().begintime().epochseconds(), 10);
                            EXPECT_EQ(service.requests.front().queryspec().timerange().endtime().epochseconds(), 20);
                            ASSERT_EQ(service.requests.front().queryspec().pvselector().pvnamelist().pvnames_size(), 1);
                            EXPECT_EQ(service.requests.front().queryspec().pvselector().pvnamelist().pvnames(0), "TEST:PV");
                            service.release_response = true;
                            lock.unlock();
                            service.condition.notify_all();
                        });
    EXPECT_THROW((void)client.executeStream("mldp.time_series", predicates, {}, context), std::runtime_error);
    driver.join();
    {
        std::unique_lock lock(service.mutex);
        ASSERT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.requests.size() == 2; }));
        EXPECT_EQ(service.requests[1].executionoptions().pagetoken(), "page2");
    }
    server->Shutdown();
}

TEST(MLDPQueryClientTest, BidiStreamReportsCursorProgress)
{
    BidiQueryService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    auto progress = std::make_shared<QueryProgressTracker>();
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .progress = progress};
    std::thread driver([&]
                        {
                            std::unique_lock lock(service.mutex);
                            ASSERT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.initial_request_received; }));
                            service.release_response = true;
                            lock.unlock();
                            service.condition.notify_all();
                        });
    auto stream = client.executeStream("mldp.time_series",
                                       {{.column = "pv", .op = PredicateOp::EQ, .values = {std::string("TEST:PV")}}}, {}, context);
    driver.join();

    ASSERT_NE(stream->next(), nullptr);
    EXPECT_EQ(stream->next(), nullptr);

    const auto snapshot = progress->snapshot();
    EXPECT_EQ(snapshot.table_name, "mldp.time_series");
    EXPECT_EQ(snapshot.operation, "MLDP queryBuckets (paged)");
    EXPECT_EQ(snapshot.cursor_responses, 1U);
    EXPECT_EQ(snapshot.cursor_next_requests, 2U);
    EXPECT_EQ(snapshot.stream_batches, 1U);
    server->Shutdown();
}

TEST(MLDPQueryClientTest, BidiStreamDestructionCancelsBlockedServerCursor)
{
    BidiQueryService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    auto cancellation = std::make_shared<QueryCancellation>();
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .cancellation = cancellation};

    // executeStream() now blocks inside the RPC until the whole cursor drains,
    // so the only way to observe the server-side cancel is to request it from
    // another thread while executeStream() is still blocked on the server.
    std::thread canceller([&]
                           {
                               std::unique_lock lock(service.mutex);
                               ASSERT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.initial_request_received; }));
                               lock.unlock();
                               cancellation->requestCancel();
                           });
    EXPECT_THROW((void)client.executeStream("mldp.time_series",
                                            {{.column = "pv", .op = PredicateOp::EQ, .values = {std::string("TEST:PV")}}}, {}, context),
                 QueryCancelled);
    canceller.join();
    {
        std::unique_lock lock(service.mutex);
        EXPECT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.client_cancelled; }));
    }
    server->Shutdown();
}

TEST(MLDPQueryClientTest, FormatterWritesJsonForEagerlyDrainedBidiStream)
{
    // executeStream() eagerly drains the whole bidi cursor into memory before
    // returning, so formatQueryStream() no longer observes per-batch
    // interleaving with cursor-next requests; it just replays the
    // materialized result. This test verifies the formatter still produces
    // output for it.
    BidiQueryService service;
    ObservingStreambuf output_buffer;
    std::ostream output(&output_buffer);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool()};
    std::thread driver([&]
                        {
                            std::unique_lock lock(service.mutex);
                            ASSERT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.initial_request_received; }));
                            service.release_response = true;
                            lock.unlock();
                            service.condition.notify_all();
                        });
    auto stream = client.executeStream("mldp.time_series",
                                       {{.column = "pv", .op = PredicateOp::EQ, .values = {std::string("TEST:PV")}}}, {}, context);
    driver.join();
    mldp_pvxs_driver::cli::formatQueryStream(*stream, mldp_pvxs_driver::cli::QueryOutputFormat::Json, output);
    EXPECT_TRUE(output_buffer.wrote.load(std::memory_order_acquire));
    server->Shutdown();
}

TEST(MLDPQueryClientTest, MaterializesMetadataVirtualColumnsWithACommonPageSchema)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient              client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext       context{.pool = arrow::default_memory_pool(), .join_batch_size = 1};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MAG:ONE")}},
        {.column = "time", .op = PredicateOp::GTE, .values = {int64_t{0}}},
    };

    const auto result = executeAll(client, "mldp.time_series", predicates, {"pv", "attributes.unrecorded", "provenance.source", "provenance.process"}, context);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->num_rows(), 2);

    const auto attributes_index = result->schema()->GetFieldIndex("attributes");
    const auto tags_index = result->schema()->GetFieldIndex("tags");
    ASSERT_GE(attributes_index, 0);
    ASSERT_GE(tags_index, 0);
    EXPECT_EQ(result->column(attributes_index)->type_id(), arrow::Type::MAP);
    EXPECT_EQ(result->column(tags_index)->type_id(), arrow::Type::LIST);

    const auto location_index = result->schema()->GetFieldIndex("attributes.location");
    const auto ordinal_index = result->schema()->GetFieldIndex("attributes.ordinal");
    const auto source_index = result->schema()->GetFieldIndex("provenance.source");
    const auto process_index = result->schema()->GetFieldIndex("provenance.process");
    const auto unrecorded_index = result->schema()->GetFieldIndex("attributes.unrecorded");
    ASSERT_GE(location_index, 0);
    ASSERT_GE(ordinal_index, 0);
    ASSERT_GE(source_index, 0);
    ASSERT_GE(process_index, 0);
    ASSERT_GE(unrecorded_index, 0);

    const auto location = std::static_pointer_cast<arrow::StringArray>(result->column(location_index));
    const auto ordinal = std::static_pointer_cast<arrow::StringArray>(result->column(ordinal_index));
    const auto source = std::static_pointer_cast<arrow::StringArray>(result->column(source_index));
    const auto process = std::static_pointer_cast<arrow::StringArray>(result->column(process_index));
    const auto unrecorded = std::static_pointer_cast<arrow::StringArray>(result->column(unrecorded_index));
    EXPECT_TRUE(location->IsNull(0));
    EXPECT_EQ(location->GetString(1), "LTU");
    EXPECT_EQ(ordinal->GetString(0), "1");
    EXPECT_TRUE(ordinal->IsNull(1));
    EXPECT_EQ(source->GetString(0), "ioc-a");
    EXPECT_TRUE(source->IsNull(1));
    EXPECT_EQ(process->GetString(0), "deterministic-sine");
    EXPECT_EQ(process->GetString(1), "archiver");
    EXPECT_TRUE(unrecorded->IsNull(0));
    EXPECT_TRUE(unrecorded->IsNull(1));

    server->Shutdown();
}

TEST(MLDPQueryClientTest, MaterializesNativeWideTableInRequestedPvOrderWithFieldMetadata)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 1};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("MAG:ONE"), std::string("RF:ONE")}},
        {.column = "time", .op = PredicateOp::GTE, .values = {int64_t{0}}},
        {.column = "time", .op = PredicateOp::LTE, .values = {int64_t{3}}},
    };

    const auto result = executeAll(client, "mldp.time_series_table", predicates, {}, context);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->num_rows(), 2);
    ASSERT_EQ(result->schema()->field_names(), std::vector<std::string>({"time", "MAG:ONE", "RF:ONE"}));
    EXPECT_EQ(result->column(0)->type_id(), arrow::Type::TIMESTAMP);
    EXPECT_EQ(result->column(1)->type_id(), arrow::Type::DOUBLE);
    EXPECT_EQ(result->column(2)->type_id(), arrow::Type::STRING);
    const auto magnet_metadata = result->schema()->field(1)->metadata();
    const auto rf_metadata = result->schema()->field(2)->metadata();
    ASSERT_NE(magnet_metadata, nullptr);
    ASSERT_NE(rf_metadata, nullptr);
    EXPECT_EQ(magnet_metadata->Get("tags").ValueOrDie(), "magnet");
    EXPECT_EQ(magnet_metadata->Get("attributes.device_group").ValueOrDie(), "MAGNET");
    EXPECT_EQ(magnet_metadata->Get("attributes.location").ValueOrDie(), "LTU");
    EXPECT_EQ(magnet_metadata->Get("provenance.process").ValueOrDie(), "archiver");
    EXPECT_EQ(rf_metadata->Get("tags").ValueOrDie(), "rf,sample,mldp_sample");
    EXPECT_EQ(rf_metadata->Get("attributes.namespace").ValueOrDie(), "mldp_sample");
    EXPECT_EQ(rf_metadata->Get("attributes.ordinal").ValueOrDie(), "1");
    EXPECT_EQ(rf_metadata->Get("attributes.sample_period_seconds").ValueOrDie(), "1");
    EXPECT_EQ(rf_metadata->Get("provenance.source").ValueOrDie(), "ioc-a");
    EXPECT_EQ(rf_metadata->Get("provenance.process").ValueOrDie(), "deterministic-sine");

    const auto time = std::static_pointer_cast<arrow::TimestampArray>(result->column(0));
    const auto magnet = std::static_pointer_cast<arrow::DoubleArray>(result->column(1));
    const auto rf = std::static_pointer_cast<arrow::StringArray>(result->column(2));
    EXPECT_DOUBLE_EQ(magnet->Value(0), 2.5);
    EXPECT_TRUE(magnet->IsNull(1));
    EXPECT_EQ(rf->GetString(0), "one");
    EXPECT_TRUE(rf->IsNull(1));

    EXPECT_EQ(time->Value(0), 1'000'000'000);
    EXPECT_EQ(time->Value(1), 2'000'000'000);
    {
        const std::lock_guard lock(service.mutex);
        const auto& pvnames = service.last_request.queryspec().pvselector().pvnamelist().pvnames();
        ASSERT_EQ(pvnames.size(), 2);
        EXPECT_EQ(pvnames.Get(0), "MAG:ONE");
        EXPECT_EQ(pvnames.Get(1), "RF:ONE");
        EXPECT_EQ(service.last_request.queryspec().timerange().begintime().epochseconds(), 0);
        EXPECT_EQ(service.last_request.queryspec().timerange().endtime().epochseconds(), 3);
    }

    server->Shutdown();
}

TEST(MLDPQueryClientTest, SendsLiteralWindowBoundsToWideTableRequest)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 1};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("MAG:ONE")}},
        {.column = "time", .op = PredicateOp::GTE, .values = {int64_t{10}}},
        {.column = "time", .op = PredicateOp::LTE, .values = {int64_t{20}}},
    };

    ASSERT_NE(executeAll(client, "mldp.time_series_table", predicates, {}, context), nullptr);
    {
        const std::lock_guard lock(service.mutex);
        EXPECT_EQ(service.last_request.queryspec().timerange().begintime().epochseconds(), 10);
        EXPECT_EQ(service.last_request.queryspec().timerange().endtime().epochseconds(), 20);
    }
    server->Shutdown();
}

TEST(MLDPQueryClientTest, SendsLiteralWindowBoundsToLongTableRequest)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 1};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("MAG:ONE")}},
        {.column = "time", .op = PredicateOp::GTE, .values = {int64_t{10}}},
        {.column = "time", .op = PredicateOp::LTE, .values = {int64_t{20}}},
    };

    ASSERT_NE(executeAll(client, "mldp.time_series", predicates, {}, context), nullptr);
    {
        const std::lock_guard lock(service.mutex);
        ASSERT_TRUE(service.last_bidi_request.has_queryspec());
        EXPECT_EQ(service.last_bidi_request.queryspec().timerange().begintime().epochseconds(), 10);
        EXPECT_EQ(service.last_bidi_request.queryspec().timerange().endtime().epochseconds(), 20);
    }
    server->Shutdown();
}

TEST(MLDPQueryClientTest, CancelsInFlightQueryTableRpc)
{
    QueryService service;
    service.block_query_table = true;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    auto cancellation = std::make_shared<QueryCancellation>();
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .cancellation = cancellation};
    const std::vector<Predicate> predicates = {{.column = "pv", .op = PredicateOp::EQ, .values = {std::string("MAG:ONE")}}};
    auto query = std::async(std::launch::async,
                            [&] { return executeAll(client, "mldp.time_series_table", predicates, {}, context); });
    {
        std::unique_lock lock(service.mutex);
        ASSERT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.query_started; }));
    }
    cancellation->requestCancel();
    EXPECT_THROW(query.get(), QueryCancelled);
    {
        std::unique_lock lock(service.mutex);
        EXPECT_TRUE(service.condition.wait_for(lock, std::chrono::seconds{2}, [&] { return service.client_cancelled; }));
    }
    server->Shutdown();
}

TEST(MLDPQueryClientTest, MapsEveryScalarNativeTypeForWideAndLongTables)
{
    QueryService        service;
    service.scalar_columns = true;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 32};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("BOOL"), std::string("U32"), std::string("U64"), std::string("I32"), std::string("I64"), std::string("FLOAT"), std::string("BINARY"), std::string("TIMESTAMP")}},
        {.column = "time", .op = PredicateOp::GTE, .values = {int64_t{0}}},
    };

    const auto wide = executeAll(client, "mldp.time_series_table", predicates, {}, context);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->schema()->field(1)->type()->id(), arrow::Type::BOOL);
    EXPECT_EQ(wide->schema()->field(2)->type()->id(), arrow::Type::UINT32);
    EXPECT_EQ(wide->schema()->field(3)->type()->id(), arrow::Type::UINT64);
    EXPECT_EQ(wide->schema()->field(4)->type()->id(), arrow::Type::INT32);
    EXPECT_EQ(wide->schema()->field(5)->type()->id(), arrow::Type::INT64);
    EXPECT_EQ(wide->schema()->field(6)->type()->id(), arrow::Type::FLOAT);
    EXPECT_EQ(wide->schema()->field(7)->type()->id(), arrow::Type::BINARY);
    EXPECT_EQ(wide->schema()->field(8)->type()->id(), arrow::Type::TIMESTAMP);

    const auto long_form = executeAll(client, "mldp.time_series", predicates, {}, context);
    ASSERT_NE(long_form, nullptr);
    const auto type_index = long_form->schema()->GetFieldIndex("column_type");
    ASSERT_GE(type_index, 0);
    const auto types = std::static_pointer_cast<arrow::StringArray>(long_form->column(type_index));
    EXPECT_EQ(types->GetString(0), "bool");
    EXPECT_EQ(types->GetString(7), "timestamp");

    server->Shutdown();
}

TEST(MLDPQueryClientTest, FiltersWholeNativeColumnsUsingTypeAndMetadataPredicates)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 32};
    const std::vector<Predicate> predicates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("MAG:ONE"), std::string("RF:ONE")}},
        {.column = "column_type", .op = PredicateOp::EQ, .values = {std::string("double")}},
        {.column = "tag", .op = PredicateOp::EQ, .values = {std::string("magnet")}},
        {.column = "attributes.device_group", .op = PredicateOp::EQ, .values = {std::string("MAGNET")}},
        {.column = "provenance.process", .op = PredicateOp::EQ, .values = {std::string("archiver")}},
    };

    const auto wide = executeAll(client, "mldp.time_series_table", predicates, {}, context);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->schema()->field_names(), std::vector<std::string>({"time", "MAG:ONE"}));

    const auto long_form = executeAll(client, "mldp.time_series", predicates, {}, context);
    ASSERT_NE(long_form, nullptr);
    ASSERT_EQ(long_form->num_rows(), 1);
    const auto pv = std::static_pointer_cast<arrow::StringArray>(long_form->GetColumnByName("pv"));
    const auto type = std::static_pointer_cast<arrow::StringArray>(long_form->GetColumnByName("column_type"));
    EXPECT_EQ(pv->GetString(0), "MAG:ONE");
    EXPECT_EQ(type->GetString(0), "double");

    server->Shutdown();
}

TEST(MLDPQueryClientTest, SelectsWideCandidateColumnsWithMetadataTextPatterns)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 32};
    const std::vector<Predicate> candidates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MAG:ONE")}},
    };
    const auto expect_mag_one = [&](const Predicate predicate)
    {
        auto predicates = candidates;
        predicates.push_back(predicate);
        const auto result = executeAll(client, "mldp.time_series_table", predicates, {}, context);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->schema()->field_names(), std::vector<std::string>({"time", "MAG:ONE"}));
    };

    expect_mag_one({.column = "attributes.device_group", .op = PredicateOp::PREFIX, .values = {std::string("MAG")}});
    expect_mag_one({.column = "attributes.device_group", .op = PredicateOp::CONTAINS, .values = {std::string("AGNE")}});
    expect_mag_one({.column = "provenance.process", .op = PredicateOp::LIKE, .values = {std::string("arch*")}});

    const auto no_match = executeAll(client, 
        "mldp.time_series_table",
        {{.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MAG:ONE")} },
         {.column = "attributes.missing", .op = PredicateOp::PREFIX, .values = {std::string("anything")}},
        },
        {},
        context);
    EXPECT_EQ(no_match, nullptr);

    server->Shutdown();
}

TEST(MLDPQueryClientTest, MaterializesAllNullWideColumnsAndHandlesEmptySelections)
{
    QueryService        service;
    service.all_null_column = true;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 32};
    const std::vector<Predicate> all_null = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("ALL:NULL")}},
    };
    const auto result = executeAll(client, "mldp.time_series_table", all_null, {}, context);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->schema()->field_names(), std::vector<std::string>({"time", "ALL:NULL"}));
    EXPECT_EQ(result->column(1)->type_id(), arrow::Type::NA);
    EXPECT_EQ(result->column(1)->null_count(), 2);

    const std::vector<Predicate> missing = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("MISSING:PV")}},
    };
    const auto empty_wide = executeAll(client, "mldp.time_series_table", missing, {}, context);
    EXPECT_EQ(empty_wide, nullptr);

    const auto empty_long = executeAll(client, "mldp.time_series", missing, {"pv", "provenance.source"}, context);
    EXPECT_EQ(empty_long, nullptr);

    const std::vector<Predicate> partial = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MISSING:PV")}},
    };
    const auto partial_wide = executeAll(client, "mldp.time_series_table", partial, {}, context);
    ASSERT_NE(partial_wide, nullptr);
    EXPECT_EQ(partial_wide->schema()->field_names(), std::vector<std::string>({"time", "RF:ONE"}));

    server->Shutdown();
}

TEST(MLDPQueryClientTest, RejectsDuplicateAndOversizedWideResponseColumns)
{
    QueryService        service;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPQueryClient        client(makeQueryConfig("127.0.0.1:" + std::to_string(port)));
    const ExecutionContext context{.pool = arrow::default_memory_pool(), .join_batch_size = 32};
    const std::vector<Predicate> duplicate_predicates = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MAG:ONE")}},
    };
    service.duplicate_column = true;
    EXPECT_THROW((void)executeAll(client, "mldp.time_series_table", duplicate_predicates, {}, context), std::runtime_error);

    service.duplicate_column = false;
    service.oversized_column = true;
    const std::vector<Predicate> oversized_predicates = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("TOO:LONG")}},
    };
    EXPECT_THROW((void)executeAll(client, "mldp.time_series_table", oversized_predicates, {}, context), std::runtime_error);

    server->Shutdown();
}
