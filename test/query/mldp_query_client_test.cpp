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
#include <query/QueryResult.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include "../config/test_config_helpers.h"

#include <gtest/gtest.h>

#include <arrow/array.h>
#include <arrow/type.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <query.grpc.pb.h>

#include <memory>
#include <string>
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
    grpc::Status queryTable(grpc::ServerContext*,
                            const dp::service::query::QueryTableRequest*,
                            dp::service::query::QueryTableResponse* response) override
    {
        auto* table = response->mutable_tableresult()->mutable_columntable();
        auto* timestamps = table->mutable_datatimestamps()->mutable_timestamplist();
        timestamps->add_timestamps()->set_epochseconds(1);
        timestamps->add_timestamps()->set_epochseconds(2);

        auto* first = table->add_datacolumns();
        first->set_name("RF:ONE");
        first->mutable_metadata()->add_tags("rf");
        first->mutable_metadata()->mutable_provenance()->set_source("ioc-a");
        addAttribute(first, "device_group", "RF");
        addAttribute(first, "ordinal", "1");
        first->add_datavalues()->set_stringvalue("one");

        auto* second = table->add_datacolumns();
        second->set_name("MAG:ONE");
        second->mutable_metadata()->add_tags("magnet");
        second->mutable_metadata()->mutable_provenance()->set_process("archiver");
        addAttribute(second, "device_group", "MAGNET");
        addAttribute(second, "location", "LTU");
        second->add_datavalues()->set_stringvalue("two");
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

    const auto first_page = client.execute("mldp.time_series", predicates, {"pv"}, context);
    ASSERT_NE(first_page.batch, nullptr);
    ASSERT_EQ(first_page.batch->num_rows(), 1);
    ASSERT_EQ(first_page.next_page_token, "ts:1");
    const auto second_page = client.execute("mldp.time_series", predicates, {"pv"}, context, first_page.next_page_token);
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
    ASSERT_GE(location_index, 0);
    ASSERT_GE(ordinal_index, 0);
    ASSERT_GE(source_index, 0);
    ASSERT_GE(process_index, 0);

    const auto first_location = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(location_index));
    const auto second_location = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(location_index));
    const auto first_ordinal = std::static_pointer_cast<arrow::StringArray>(first_page.batch->column(ordinal_index));
    const auto second_ordinal = std::static_pointer_cast<arrow::StringArray>(second_page.batch->column(ordinal_index));
    EXPECT_TRUE(first_location->IsNull(0));
    EXPECT_EQ(second_location->GetString(0), "LTU");
    EXPECT_EQ(first_ordinal->GetString(0), "1");
    EXPECT_TRUE(second_ordinal->IsNull(0));

    server->Shutdown();
}
