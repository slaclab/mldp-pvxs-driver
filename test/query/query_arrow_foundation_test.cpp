//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/QuerySubcommand.h>
#include <query/ArrowTypeMap.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include <arrow/api.h>
#include <arrow/filesystem/mockfs.h>
#include <gtest/gtest.h>

#include <chrono>
#include <sstream>

using namespace mldp_pvxs_driver;

namespace {

TEST(ArrowTypeMapTest, MapsEveryColumnType)
{
    EXPECT_TRUE(query::arrowType(query::ColumnType::STRING)->Equals(*arrow::utf8()));
    EXPECT_TRUE(query::arrowType(query::ColumnType::TIMESTAMP)->Equals(*arrow::timestamp(arrow::TimeUnit::SECOND, "UTC")));
    EXPECT_TRUE(query::arrowType(query::ColumnType::DURATION_SECONDS)->Equals(*arrow::duration(arrow::TimeUnit::SECOND)));
    EXPECT_TRUE(query::arrowType(query::ColumnType::INT)->Equals(*arrow::int64()));
    EXPECT_TRUE(query::arrowType(query::ColumnType::BOOL)->Equals(*arrow::boolean()));
}

TEST(SpillManagerTest, RoundTripsAndDeletesAfterConsumption)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    query::SpillManager manager(file_system, "spill");
    const auto schema = arrow::schema({arrow::field("value", arrow::int64())});
    arrow::Int64Builder builder;
    ASSERT_TRUE(builder.AppendValues({1, 2}).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(builder.Finish(&values).ok());
    const auto batch = arrow::RecordBatch::Make(schema, 2, {values});

    auto spilled = manager.spill("q", {batch, batch});
    ASSERT_TRUE(spilled.ok()) << spilled.status().ToString();
    const auto handle = *spilled;
    EXPECT_EQ(handle.batch_count, 2);
    auto reader_result = manager.read(handle);
    ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
    auto reader = std::move(*reader_result);
    auto first_result = reader.next();
    ASSERT_TRUE(first_result.ok()) << first_result.status().ToString();
    const auto read_first = *first_result;
    ASSERT_NE(read_first, nullptr);
    auto second_result = reader.next();
    ASSERT_TRUE(second_result.ok()) << second_result.status().ToString();
    const auto read_second = *second_result;
    ASSERT_NE(read_second, nullptr);
    const auto file_info = file_system->GetFileInfo(handle.path);
    ASSERT_TRUE(file_info.ok()) << file_info.status().ToString();
    EXPECT_EQ(file_info->type(), arrow::fs::FileType::NotFound);
}

TEST(SpillManagerTest, CleanupDeletesOutstandingFiles)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    query::SpillManager manager(file_system, "spill");
    arrow::Int64Builder builder;
    ASSERT_TRUE(builder.Append(1).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(builder.Finish(&values).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", arrow::int64())}), 1, {values});

    auto spilled = manager.spill("q", {batch});
    ASSERT_TRUE(spilled.ok()) << spilled.status().ToString();
    const auto handle = *spilled;
    ASSERT_TRUE(manager.cleanup().ok());
    ASSERT_TRUE(manager.cleanup().ok());
    const auto file_info = file_system->GetFileInfo(handle.path);
    ASSERT_TRUE(file_info.ok()) << file_info.status().ToString();
    EXPECT_EQ(file_info->type(), arrow::fs::FileType::NotFound);
}

TEST(QuerySubcommandTest, PreparesBothSupportedQueryableShapes)
{
    query::QueryableFactory::instance().reset();
    const auto typed = config::Config::configFromYamlString("queryable:\n  - type: mldp\n    provider-name: test\n    ingestion-url: localhost:1\n    query-url: localhost:2\n");
    cli::prepareQuerySubcommand(typed);
    EXPECT_TRUE(query::QueryableFactory::instance().isPrepared<query::impl::mldp::MLDPQueryClient>());

    const auto keyed = config::Config::configFromYamlString("queryable:\n  mldp:\n    provider-name: test\n    ingestion-url: localhost:1\n    query-url: localhost:2\n");
    cli::prepareQuerySubcommand(keyed);
    EXPECT_TRUE(query::QueryableFactory::instance().isPrepared<query::impl::mldp::MLDPQueryClient>());
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, ReplReportsUnimplementedQueriesAndExits)
{
    std::istringstream input("select * from mldp.pv_stats\nexit\n");
    std::ostringstream output;
    EXPECT_EQ(cli::runQueryRepl(input, output), 0);
    EXPECT_EQ(output.str(), "Query execution is not implemented yet.\n");
}

TEST(QuerySubcommandTest, ReplReportsParseErrors)
{
    std::istringstream input("select from mldp.pv_stats\nexit\n");
    std::ostringstream output;
    EXPECT_EQ(cli::runQueryRepl(input, output), 0);
    EXPECT_NE(output.str().find("Parse error at "), std::string::npos);
}

} // namespace
