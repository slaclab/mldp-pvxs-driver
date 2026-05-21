//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>

#include "../config/test_config_helpers.h"

#include <gtest/gtest.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;
using mldp_pvxs_driver::config::makeConfigFromYaml;

static mldp_pvxs_driver::config::Config make_annotation_config()
{
    return makeConfigFromYaml(
        "provider-name: test-provider\n"
        "ingestion-url: localhost:19999\n"
        "query-url: localhost:19998\n"
        "annotation-url: localhost:19997\n"
        "min-conn: 1\n"
        "max-conn: 1\n");
}

class MLDPAnnotationQueryClientTest : public ::testing::Test {
protected:
    void SetUp() override   { QueryableFactory::instance().reset(); }
    void TearDown() override { QueryableFactory::instance().reset(); }
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
    MLDPAnnotationQueryClient client(make_annotation_config());
    dp::service::common::Timestamp ts;
    ts.set_epochseconds(0);
    ts.set_nanoseconds(0);
    EXPECT_NO_THROW({
        auto result = client.getActiveConfigurations(ts);
        (void)result;
    });
}
