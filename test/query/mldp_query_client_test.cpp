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
#include <query/QueryResult.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include "../config/test_config_helpers.h"

#include <gtest/gtest.h>

#include <arrow/array.h>
#include <arrow/type.h>
#include <arrow/util/key_value_metadata.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <query.grpc.pb.h>

#include <memory>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;

namespace {

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
    dp::service::query::QueryTableRequest last_request;
    bool                                  duplicate_column{false};
    bool                                  oversized_column{false};
    bool                                  scalar_columns{false};
    bool                                  all_null_column{false};
    bool                                  block_query_table{false};
    bool                                  query_started{false};
    bool                                  client_cancelled{false};
    mutable std::mutex                    mutex;
    std::condition_variable               condition;

    grpc::Status queryTable(grpc::ServerContext* context,
                            const dp::service::query::QueryTableRequest* request,
                            dp::service::query::QueryTableResponse* response) override
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
        auto* table = response->mutable_tableresult()->mutable_columntable();
        auto* timestamps = table->mutable_datatimestamps()->mutable_timestamplist();
        timestamps->add_timestamps()->set_epochseconds(1);
        timestamps->add_timestamps()->set_epochseconds(2);

        auto* first = table->add_datacolumns();
        first->set_name("RF:ONE");
        first->mutable_metadata()->add_tags("rf");
        first->mutable_metadata()->add_tags("sample");
        first->mutable_metadata()->add_tags("mldp_sample");
        first->mutable_metadata()->mutable_provenance()->set_source("ioc-a");
        first->mutable_metadata()->mutable_provenance()->set_process("deterministic-sine");
        addAttribute(first, "device_group", "RF");
        addAttribute(first, "ordinal", "1");
        addAttribute(first, "namespace", "mldp_sample");
        addAttribute(first, "sample_period_seconds", "1");
        first->add_datavalues()->set_stringvalue("one");

        auto* second = table->add_datacolumns();
        second->set_name("MAG:ONE");
        second->mutable_metadata()->add_tags("magnet");
        second->mutable_metadata()->mutable_provenance()->set_process("archiver");
        addAttribute(second, "device_group", "MAGNET");
        addAttribute(second, "location", "LTU");
        second->add_datavalues()->set_doublevalue(2.5);

        if (duplicate_column)
        {
            auto* duplicate = table->add_datacolumns();
            duplicate->set_name("RF:ONE");
            duplicate->add_datavalues()->set_stringvalue("duplicate");
        }
        if (oversized_column)
        {
            auto* oversized = table->add_datacolumns();
            oversized->set_name("TOO:LONG");
            oversized->add_datavalues()->set_intvalue(1);
            oversized->add_datavalues()->set_intvalue(2);
            oversized->add_datavalues()->set_intvalue(3);
        }
        if (scalar_columns)
        {
            const auto add_column = [table](const std::string& name, const auto& set_value)
            {
                auto* column = table->add_datacolumns();
                column->set_name(name);
                set_value(column->add_datavalues());
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
            auto* column = table->add_datacolumns();
            column->set_name("ALL:NULL");
            column->add_datavalues();
        }
        return grpc::Status::OK;
    }
};

} // namespace

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

    const auto first_page = client.execute("mldp.time_series", predicates, {"pv", "attributes.unrecorded", "provenance.source", "provenance.process"}, context);
    ASSERT_NE(first_page.batch, nullptr);
    ASSERT_EQ(first_page.batch->num_rows(), 1);
    ASSERT_EQ(first_page.next_page_token, "ts:1");
    const auto second_page = client.execute("mldp.time_series", predicates, {"pv", "attributes.unrecorded", "provenance.source", "provenance.process"}, context, first_page.next_page_token);
    ASSERT_NE(second_page.batch, nullptr);
    ASSERT_EQ(second_page.batch->num_rows(), 1);
    EXPECT_TRUE(second_page.next_page_token.empty());

    EXPECT_TRUE(first_page.batch->schema()->Equals(*second_page.batch->schema()));
    const auto attributes_index = first_page.batch->schema()->GetFieldIndex("attributes");
    const auto tags_index = first_page.batch->schema()->GetFieldIndex("tags");
    ASSERT_GE(attributes_index, 0);
    ASSERT_GE(tags_index, 0);
    EXPECT_EQ(first_page.batch->column(attributes_index)->type_id(), arrow::Type::MAP);
    EXPECT_EQ(first_page.batch->column(tags_index)->type_id(), arrow::Type::LIST);

    const auto location_index = first_page.batch->schema()->GetFieldIndex("attributes.location");
    const auto ordinal_index = first_page.batch->schema()->GetFieldIndex("attributes.ordinal");
    const auto source_index = first_page.batch->schema()->GetFieldIndex("provenance.source");
    const auto process_index = first_page.batch->schema()->GetFieldIndex("provenance.process");
    const auto unrecorded_index = first_page.batch->schema()->GetFieldIndex("attributes.unrecorded");
    ASSERT_GE(location_index, 0);
    ASSERT_GE(ordinal_index, 0);
    ASSERT_GE(source_index, 0);
    ASSERT_GE(process_index, 0);
    ASSERT_GE(unrecorded_index, 0);

    const auto first_location = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(location_index));
    const auto second_location = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(location_index));
    const auto first_ordinal = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(ordinal_index));
    const auto second_ordinal = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(ordinal_index));
    const auto first_source = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(source_index));
    const auto second_source = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(source_index));
    const auto first_process = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(process_index));
    const auto second_process = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(process_index));
    const auto first_unrecorded = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(unrecorded_index));
    const auto second_unrecorded = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(unrecorded_index));
    EXPECT_TRUE(first_location->IsNull(0));
    EXPECT_EQ(second_location->GetString(0), "LTU");
    EXPECT_EQ(first_ordinal->GetString(0), "1");
    EXPECT_TRUE(second_ordinal->IsNull(0));
    EXPECT_EQ(first_source->GetString(0), "ioc-a");
    EXPECT_TRUE(second_source->IsNull(0));
    EXPECT_EQ(first_process->GetString(0), "deterministic-sine");
    EXPECT_EQ(second_process->GetString(0), "archiver");
    EXPECT_TRUE(first_unrecorded->IsNull(0));
    EXPECT_TRUE(second_unrecorded->IsNull(0));

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

    const auto result = client.execute("mldp.time_series_table", predicates, {}, context);
    ASSERT_NE(result.batch, nullptr);
    EXPECT_TRUE(result.next_page_token.empty());
    EXPECT_EQ(result.batch->num_rows(), 2);
    ASSERT_EQ(result.batch->schema()->field_names(), std::vector<std::string>({"time", "MAG:ONE", "RF:ONE"}));
    EXPECT_EQ(result.batch->column(0)->type_id(), arrow::Type::TIMESTAMP);
    EXPECT_EQ(result.batch->column(1)->type_id(), arrow::Type::DOUBLE);
    EXPECT_EQ(result.batch->column(2)->type_id(), arrow::Type::STRING);
    const auto magnet_metadata = result.batch->schema()->field(1)->metadata();
    const auto rf_metadata = result.batch->schema()->field(2)->metadata();
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

    const auto time = std::static_pointer_cast<arrow::TimestampArray>(result.batch->column(0));
    const auto magnet = std::static_pointer_cast<arrow::DoubleArray>(result.batch->column(1));
    const auto rf = std::static_pointer_cast<arrow::StringArray>(result.batch->column(2));
    EXPECT_DOUBLE_EQ(magnet->Value(0), 2.5);
    EXPECT_TRUE(magnet->IsNull(1));
    EXPECT_EQ(rf->GetString(0), "one");
    EXPECT_TRUE(rf->IsNull(1));

    EXPECT_EQ(time->Value(0), 1'000'000'000);
    EXPECT_EQ(time->Value(1), 2'000'000'000);
    {
        const std::lock_guard lock(service.mutex);
        EXPECT_EQ(service.last_request.format(), dp::service::query::QueryTableRequest::TABLE_FORMAT_COLUMN);
        ASSERT_EQ(service.last_request.pvnamelist().pvnames_size(), 2);
        EXPECT_EQ(service.last_request.pvnamelist().pvnames(0), "MAG:ONE");
        EXPECT_EQ(service.last_request.pvnamelist().pvnames(1), "RF:ONE");
        EXPECT_EQ(service.last_request.begintime().epochseconds(), 0);
        EXPECT_EQ(service.last_request.endtime().epochseconds(), 3);
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

    ASSERT_NE(client.execute("mldp.time_series_table", predicates, {}, context).batch, nullptr);
    {
        const std::lock_guard lock(service.mutex);
        EXPECT_EQ(service.last_request.begintime().epochseconds(), 10);
        EXPECT_EQ(service.last_request.endtime().epochseconds(), 20);
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

    ASSERT_NE(client.execute("mldp.time_series", predicates, {}, context).batch, nullptr);
    {
        const std::lock_guard lock(service.mutex);
        EXPECT_EQ(service.last_request.begintime().epochseconds(), 10);
        EXPECT_EQ(service.last_request.endtime().epochseconds(), 20);
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
                            [&] { return client.execute("mldp.time_series_table", predicates, {}, context); });
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

    const auto wide = client.execute("mldp.time_series_table", predicates, {}, context);
    ASSERT_NE(wide.batch, nullptr);
    EXPECT_EQ(wide.batch->schema()->field(1)->type()->id(), arrow::Type::BOOL);
    EXPECT_EQ(wide.batch->schema()->field(2)->type()->id(), arrow::Type::UINT32);
    EXPECT_EQ(wide.batch->schema()->field(3)->type()->id(), arrow::Type::UINT64);
    EXPECT_EQ(wide.batch->schema()->field(4)->type()->id(), arrow::Type::INT32);
    EXPECT_EQ(wide.batch->schema()->field(5)->type()->id(), arrow::Type::INT64);
    EXPECT_EQ(wide.batch->schema()->field(6)->type()->id(), arrow::Type::FLOAT);
    EXPECT_EQ(wide.batch->schema()->field(7)->type()->id(), arrow::Type::BINARY);
    EXPECT_EQ(wide.batch->schema()->field(8)->type()->id(), arrow::Type::TIMESTAMP);

    const auto long_form = client.execute("mldp.time_series", predicates, {}, context);
    ASSERT_NE(long_form.batch, nullptr);
    const auto type_index = long_form.batch->schema()->GetFieldIndex("column_type");
    ASSERT_GE(type_index, 0);
    const auto types = std::static_pointer_cast<arrow::StringArray>(long_form.batch->column(type_index));
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

    const auto wide = client.execute("mldp.time_series_table", predicates, {}, context);
    ASSERT_NE(wide.batch, nullptr);
    EXPECT_EQ(wide.batch->schema()->field_names(), std::vector<std::string>({"time", "MAG:ONE"}));

    const auto long_form = client.execute("mldp.time_series", predicates, {}, context);
    ASSERT_NE(long_form.batch, nullptr);
    ASSERT_EQ(long_form.batch->num_rows(), 1);
    const auto pv = std::static_pointer_cast<arrow::StringArray>(long_form.batch->GetColumnByName("pv"));
    const auto type = std::static_pointer_cast<arrow::StringArray>(long_form.batch->GetColumnByName("column_type"));
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
        const auto result = client.execute("mldp.time_series_table", predicates, {}, context);
        ASSERT_NE(result.batch, nullptr);
        EXPECT_EQ(result.batch->schema()->field_names(), std::vector<std::string>({"time", "MAG:ONE"}));
    };

    expect_mag_one({.column = "attributes.device_group", .op = PredicateOp::PREFIX, .values = {std::string("MAG")}});
    expect_mag_one({.column = "attributes.device_group", .op = PredicateOp::CONTAINS, .values = {std::string("AGNE")}});
    expect_mag_one({.column = "provenance.process", .op = PredicateOp::LIKE, .values = {std::string("arch*")}});

    const auto no_match = client.execute(
        "mldp.time_series_table",
        {{.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MAG:ONE")} },
         {.column = "attributes.missing", .op = PredicateOp::PREFIX, .values = {std::string("anything")}},
        },
        {},
        context);
    EXPECT_EQ(no_match.batch, nullptr);
    EXPECT_TRUE(no_match.next_page_token.empty());

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
    const auto result = client.execute("mldp.time_series_table", all_null, {}, context);
    ASSERT_NE(result.batch, nullptr);
    ASSERT_EQ(result.batch->schema()->field_names(), std::vector<std::string>({"time", "ALL:NULL"}));
    EXPECT_EQ(result.batch->column(1)->type_id(), arrow::Type::NA);
    EXPECT_EQ(result.batch->column(1)->null_count(), 2);

    const std::vector<Predicate> missing = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("MISSING:PV")}},
    };
    const auto empty_wide = client.execute("mldp.time_series_table", missing, {}, context);
    EXPECT_EQ(empty_wide.batch, nullptr);
    EXPECT_TRUE(empty_wide.next_page_token.empty());

    const auto empty_long = client.execute("mldp.time_series", missing, {"pv", "provenance.source"}, context);
    ASSERT_NE(empty_long.batch, nullptr);
    EXPECT_EQ(empty_long.batch->num_rows(), 0);
    EXPECT_GE(empty_long.batch->schema()->GetFieldIndex("pv"), 0);
    EXPECT_GE(empty_long.batch->schema()->GetFieldIndex("provenance.source"), 0);

    const std::vector<Predicate> partial = {
        {.column = "pv", .op = PredicateOp::IN, .values = {std::string("RF:ONE"), std::string("MISSING:PV")}},
    };
    const auto partial_wide = client.execute("mldp.time_series_table", partial, {}, context);
    ASSERT_NE(partial_wide.batch, nullptr);
    EXPECT_EQ(partial_wide.batch->schema()->field_names(), std::vector<std::string>({"time", "RF:ONE"}));

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
    EXPECT_THROW((void)client.execute("mldp.time_series_table", duplicate_predicates, {}, context), std::runtime_error);

    service.duplicate_column = false;
    service.oversized_column = true;
    const std::vector<Predicate> oversized_predicates = {
        {.column = "pv", .op = PredicateOp::EQ, .values = {std::string("TOO:LONG")}},
    };
    EXPECT_THROW((void)client.execute("mldp.time_series_table", oversized_predicates, {}, context), std::runtime_error);

    server->Shutdown();
}
