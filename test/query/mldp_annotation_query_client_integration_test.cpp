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
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>

#include "../config/test_config_helpers.h"

#include <gtest/gtest.h>

#include <arrow/array.h>
#include <arrow/type.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <memory>
#include <string>
#include <vector>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;
using mldp_pvxs_driver::config::makeConfigFromYaml;

namespace {

mldp_pvxs_driver::config::Config make_annotation_config(const std::string& annotation_url = "localhost:19997")
{
    return makeConfigFromYaml(
        "provider-name: test-provider\n" "ingestion-url: localhost:19999\n" "query-url: localhost:19998\n" "annotation-url: " + annotation_url + "\n" "min-conn: 1\n" "max-conn: 1\n");
}

void addAttribute(dp::service::common::PvMetadata* record, const std::string& name, const std::string& value)
{
    auto* attribute = record->add_attributes();
    attribute->set_name(name);
    attribute->set_value(value);
}

class PagedAnnotationService final : public dp::service::annotation::DpAnnotationService::Service
{
public:
    bool no_records{false};

    grpc::Status queryPvMetadata(
        grpc::ServerContext*,
        const dp::service::annotation::QueryPvMetadataRequest* request,
        dp::service::annotation::QueryPvMetadataResponse*      response) override
    {
        auto* result = response->mutable_pvmetadataresult();
        if (no_records)
        {
            return grpc::Status::OK;
        }
        if (request->pagetoken().empty())
        {
            auto* first = result->add_pvmetadata();
            first->set_pvname("RF:ONE");
            first->add_tags("rf");
            addAttribute(first, "device_group", "RF");
            addAttribute(first, "ordinal", "1");
            result->set_nextpagetoken("second-page");
        }
        else if (request->pagetoken() == "second-page")
        {
            auto* second = result->add_pvmetadata();
            second->set_pvname("MAG:ONE");
            second->add_tags("magnet");
            addAttribute(second, "device_group", "MAGNET");
            addAttribute(second, "location", "LTU");
        }
        return grpc::Status::OK;
    }
};

} // namespace

class MLDPAnnotationQueryClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QueryableFactory::instance().reset();
    }

    void TearDown() override
    {
        QueryableFactory::instance().reset();
    }
};

// Factory integration: prepare<MLDPAnnotationQueryClient> then create returns non-null.
TEST_F(MLDPAnnotationQueryClientTest, FactoryPrepareAndCreateReturnsNonNull)
{
    QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(make_annotation_config());
    EXPECT_TRUE(QueryableFactory::instance().isPrepared<MLDPAnnotationQueryClient>());
    auto client = QueryableFactory::instance().create<MLDPAnnotationQueryClient>();
    EXPECT_NE(client, nullptr);
}

// Factory: creating an unprepared type throws std::runtime_error.
TEST_F(MLDPAnnotationQueryClientTest, CreateOnUnpreparedTypeThrows)
{
    EXPECT_THROW(QueryableFactory::instance().create<MLDPAnnotationQueryClient>(),
                 std::runtime_error);
}

// Factory: reset clears the annotation client registration.
TEST_F(MLDPAnnotationQueryClientTest, ResetClearsRegistration)
{
    QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(make_annotation_config());
    QueryableFactory::instance().reset();
    EXPECT_FALSE(QueryableFactory::instance().isPrepared<MLDPAnnotationQueryClient>());
}

// Live RPC: getPvMetadata — requires a reachable annotation service.
// The call must not throw; on an unreachable endpoint it returns std::nullopt.
TEST_F(MLDPAnnotationQueryClientTest, GetPvMetadataDoesNotThrowOnUnreachableEndpoint)
{
    MLDPAnnotationQueryClient client(make_annotation_config());
    EXPECT_NO_THROW({
        auto result = client.getPvMetadata("TEST:PV:NAME");
        // nullopt is the expected result when the endpoint is unreachable.
        (void)result;
    });
}

// Live RPC: getActiveConfigurations — requires a reachable annotation service.
// The call must not throw; on an unreachable endpoint it returns an empty vector.
TEST_F(MLDPAnnotationQueryClientTest, GetActiveConfigurationsDoesNotThrowOnUnreachableEndpoint)
{
    MLDPAnnotationQueryClient      client(make_annotation_config());
    dp::service::common::Timestamp ts;
    ts.set_epochseconds(0);
    ts.set_nanoseconds(0);
    EXPECT_NO_THROW({
        auto result = client.getActiveConfigurations(ts);
        (void)result;
    });
}

TEST_F(MLDPAnnotationQueryClientTest, MaterializesDiscoveredAttributesAcrossAllBackendPages)
{
    PagedAnnotationService service;
    grpc::ServerBuilder    builder;
    int                    port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPAnnotationQueryClient client(make_annotation_config("127.0.0.1:" + std::to_string(port)));
    const auto                result = client.execute("mldp.pv_metadata", {}, {"pv"}, {.pool = arrow::default_memory_pool()});

    ASSERT_NE(result.batch, nullptr);
    EXPECT_TRUE(result.next_page_token.empty());
    EXPECT_EQ(result.batch->num_rows(), 2);
    const auto tags_index = result.batch->schema()->GetFieldIndex("tags");
    const auto attributes_index = result.batch->schema()->GetFieldIndex("attributes");
    ASSERT_GE(tags_index, 0);
    ASSERT_GE(attributes_index, 0);
    EXPECT_EQ(result.batch->column(tags_index)->type_id(), arrow::Type::LIST);
    EXPECT_EQ(result.batch->column(attributes_index)->type_id(), arrow::Type::MAP);
    const auto device_group_index = result.batch->schema()->GetFieldIndex("attributes.device_group");
    const auto location_index = result.batch->schema()->GetFieldIndex("attributes.location");
    const auto ordinal_index = result.batch->schema()->GetFieldIndex("attributes.ordinal");
    ASSERT_GE(device_group_index, 0);
    ASSERT_GE(location_index, 0);
    ASSERT_GE(ordinal_index, 0);

    const auto device_group = std::static_pointer_cast<arrow::StringArray>(result.batch->column(device_group_index));
    const auto location = std::static_pointer_cast<arrow::StringArray>(result.batch->column(location_index));
    const auto ordinal = std::static_pointer_cast<arrow::StringArray>(result.batch->column(ordinal_index));
    EXPECT_EQ(device_group->GetString(0), "RF");
    EXPECT_EQ(device_group->GetString(1), "MAGNET");
    EXPECT_TRUE(location->IsNull(0));
    EXPECT_EQ(location->GetString(1), "LTU");
    EXPECT_EQ(ordinal->GetString(0), "1");
    EXPECT_TRUE(ordinal->IsNull(1));

    server->Shutdown();
}

TEST_F(MLDPAnnotationQueryClientTest, EmptySelectionProducesAnEmptyBatch)
{
    PagedAnnotationService service;
    service.no_records = true;
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    MLDPAnnotationQueryClient client(make_annotation_config("127.0.0.1:" + std::to_string(port)));
    const auto result = client.execute(
        "mldp.pv_metadata",
        {{.column = "pv", .op = PredicateOp::EQ, .values = {std::string("MISSING:PV")}}},
        {"pv"},
        {.pool = arrow::default_memory_pool()});

    ASSERT_NE(result.batch, nullptr);
    EXPECT_EQ(result.batch->num_rows(), 0);
    EXPECT_TRUE(result.next_page_token.empty());
    EXPECT_EQ(result.batch->schema()->field_names(),
              std::vector<std::string>({"pv", "alias", "description", "modified_by", "created_time", "updated_time", "tags", "attributes"}));

    server->Shutdown();
}
