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
#include <query/QueryableHolder.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include "../config/test_config_helpers.h"

#include <gtest/gtest.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::impl::mldp;

static mldp_pvxs_driver::config::Config make_minimal_config()
{
    return mldp_pvxs_driver::config::makeConfigFromYaml(
        "provider-name: test-provider\n"
        "ingestion-url: localhost:19999\n"
        "query-url: localhost:19998\n"
        "min-conn: 1\n"
        "max-conn: 1\n");
}

class QueryableFactoryTest : public ::testing::Test {
protected:
    void SetUp() override   { QueryableFactory::instance().reset(); }
    void TearDown() override { QueryableFactory::instance().reset(); }
};

TEST_F(QueryableFactoryTest, IsPreparedFalseBeforePrepare) {
    EXPECT_FALSE(QueryableFactory::instance().isPrepared<MLDPQueryClient>());
}

TEST_F(QueryableFactoryTest, IsPreparedTrueAfterPrepare) {
    QueryableFactory::instance().prepare<MLDPQueryClient>(make_minimal_config());
    EXPECT_TRUE(QueryableFactory::instance().isPrepared<MLDPQueryClient>());
}

TEST_F(QueryableFactoryTest, CreateReturnsNonNull) {
    QueryableFactory::instance().prepare<MLDPQueryClient>(make_minimal_config());
    auto q = QueryableFactory::instance().create<MLDPQueryClient>();
    EXPECT_NE(q, nullptr);
}

TEST_F(QueryableFactoryTest, CreateOnUnpreparedTypeThrows) {
    EXPECT_THROW(QueryableFactory::instance().create<MLDPQueryClient>(),
                 std::runtime_error);
}

TEST_F(QueryableFactoryTest, ResetClearsRegisteredTypes) {
    QueryableFactory::instance().prepare<MLDPQueryClient>(make_minimal_config());
    QueryableFactory::instance().reset();
    EXPECT_FALSE(QueryableFactory::instance().isPrepared<MLDPQueryClient>());
}

TEST_F(QueryableFactoryTest, QueryableHolderAsReturnsNonNullForCorrectType) {
    QueryableFactory::instance().prepare<MLDPQueryClient>(make_minimal_config());
    auto q = QueryableFactory::instance().create<MLDPQueryClient>();
    QueryableHolder holder(std::move(q));
    EXPECT_TRUE(holder.valid());
    EXPECT_NE(holder.as<MLDPQueryClient>(), nullptr);
}
