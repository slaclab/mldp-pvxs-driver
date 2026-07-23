//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/QuerySubcommand.h>
#include <query/QueryFormatter.h>
#include <query/ArrowTypeMap.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>
#include <query/impl/mldp/MLDPQueryClient.h>

#include <arrow/api.h>
#include <arrow/filesystem/mockfs.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
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

TEST(SpillManagerTest, ReaderDestructorDeletesUnconsumedFile)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    query::SpillManager manager(file_system, "spill");
    arrow::Int64Builder builder;
    ASSERT_TRUE(builder.Append(7).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(builder.Finish(&values).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", arrow::int64())}), 1, {values});

    auto spilled = manager.spill("destructor", {batch});
    ASSERT_TRUE(spilled.ok()) << spilled.status().ToString();
    const auto path = spilled->path;
    {
        auto reader_result = manager.read(*spilled);
        ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
        auto reader = std::move(*reader_result);
    }
    auto file_info = file_system->GetFileInfo(path);
    ASSERT_TRUE(file_info.ok()) << file_info.status().ToString();
    EXPECT_EQ(file_info->type(), arrow::fs::FileType::NotFound);
}

TEST(QueryFormatterTest, FormatsJsonCsvTableAndArrowToTheSuppliedStream)
{
    arrow::StringBuilder text_builder;
    arrow::Int64Builder integer_builder;
    arrow::BooleanBuilder boolean_builder;
    ASSERT_TRUE(text_builder.Append("a,\"b").ok());
    ASSERT_TRUE(integer_builder.Append(42).ok());
    ASSERT_TRUE(boolean_builder.Append(true).ok());
    std::shared_ptr<arrow::Array> text;
    std::shared_ptr<arrow::Array> integer;
    std::shared_ptr<arrow::Array> boolean;
    ASSERT_TRUE(text_builder.Finish(&text).ok());
    ASSERT_TRUE(integer_builder.Finish(&integer).ok());
    ASSERT_TRUE(boolean_builder.Finish(&boolean).ok());
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("text", arrow::utf8()), arrow::field("number", arrow::int64()), arrow::field("valid", arrow::boolean())}),
        1,
        {text, integer, boolean});
    const query::QueryExecutionResult result{.batches = {batch}};

    std::ostringstream json;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Json, json);
    EXPECT_EQ(json.str(), "{\"text\":\"a,\\\"b\",\"number\":42,\"valid\":true}\n");

    std::ostringstream csv;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Csv, csv);
    EXPECT_EQ(csv.str(), "text,number,valid\n\"a,\"\"b\",42,true\n");

    std::ostringstream table;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Table, table);
    EXPECT_NE(table.str().find("text"), std::string::npos);

    std::ostringstream arrow_output;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Arrow, arrow_output);
    const auto arrow_bytes = arrow_output.str();
    ASSERT_FALSE(arrow_bytes.empty());
    auto input = std::make_shared<arrow::io::BufferReader>(arrow_bytes);
    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(input);
    ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
    auto read_batch = (*reader_result)->Next();
    ASSERT_TRUE(read_batch.ok()) << read_batch.status().ToString();
    ASSERT_NE(*read_batch, nullptr);
    EXPECT_TRUE((*read_batch)->Equals(*batch));
}

TEST(QuerySubcommandTest, PreparesBothSupportedQueryableShapes)
{
    cli::QuerySubcommandPreparer preparer;
    query::QueryableFactory::instance().reset();
    const auto typed = config::Config::configFromYamlString("queryable:\n  - type: mldp\n    provider-name: test\n    ingestion-url: localhost:1\n    query-url: localhost:2\n");
    preparer.prepare(typed);
    EXPECT_TRUE(query::QueryableFactory::instance().isPrepared<query::impl::mldp::MLDPQueryClient>());

    const auto keyed = config::Config::configFromYamlString("queryable:\n  mldp:\n    provider-name: test\n    ingestion-url: localhost:1\n    query-url: localhost:2\n");
    preparer.prepare(keyed);
    EXPECT_TRUE(query::QueryableFactory::instance().isPrepared<query::impl::mldp::MLDPQueryClient>());
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, ReplReportsPlanningOrExecutionErrorsAndExits)
{
    char arg0[] = "query";
    char arg1[] = "select * from mldp.pv_stats";
    char* argv[] = {arg0, arg1};
    cli::QuerySubcommand querySubcommand;
    std::ostringstream output;
    std::ostringstream error;
    EXPECT_EQ(querySubcommand.run(2, argv, {}, output, error), 1);
    EXPECT_TRUE(error.str().find("BindError") != std::string::npos ||
                error.str().find("Query error:") != std::string::npos);
}

TEST(QuerySubcommandTest, ReplReportsParseErrors)
{
    char arg0[] = "query";
    char arg1[] = "select from mldp.pv_stats";
    char* argv[] = {arg0, arg1};
    cli::QuerySubcommand querySubcommand;
    std::ostringstream output;
    std::ostringstream error;
    EXPECT_EQ(querySubcommand.run(2, argv, {}, output, error), 1);
    EXPECT_NE(error.str().find("Parse error at "), std::string::npos);
}

TEST(QuerySubcommandTest, RejectsLocalConfigFlag)
{
    char arg0[] = "query";
    char arg1[] = "-c";
    char arg2[] = "config.yaml";
    char arg3[] = "SHOW TABLES";
    char* argv[] = {arg0, arg1, arg2, arg3};
    cli::QuerySubcommand querySubcommand;
    std::ostringstream output;
    std::ostringstream error;
    EXPECT_EQ(querySubcommand.run(4, argv, {}, output, error), 1);
    EXPECT_NE(error.str().find("global option"), std::string::npos);
}

TEST(QuerySubcommandTest, ShowTablesFailsWithoutQueryableConfig)
{
    char arg0[] = "query";
    char arg1[] = "SHOW TABLES";
    char* argv[] = {arg0, arg1};
    cli::QuerySubcommand querySubcommand;
    std::ostringstream output;
    std::ostringstream error;
    EXPECT_EQ(querySubcommand.run(2, argv, {}, output, error), 1);
    EXPECT_NE(error.str().find("Missing 'queryable' configuration"), std::string::npos);
}

} // namespace
