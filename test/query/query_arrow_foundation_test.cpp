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
#include <query/QueryTableCatalog.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <query/parser/QueryParser.h>
#include <config/ConfigSource.h>
#include <query/planner/Binder.h>
#include <query/plan/PlannerError.h>

#include <arrow/api.h>
#include <arrow/array/builder_nested.h>
#include <arrow/array/builder_union.h>
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

TEST(QueryTableCatalogTest, PersistentTableIsDiscoveredByANewCatalogAndDroppedSafely)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    arrow::Int64Builder builder;
    ASSERT_TRUE(builder.AppendValues({4, 5}).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(builder.Finish(&values).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", arrow::int64())}), 2, {values});

    {
        query::QueryTableCatalog catalog(file_system, "catalog-root");
        ASSERT_TRUE(catalog.create("production_samples", query::TableLifetime::Persistent, {batch}).ok());
        EXPECT_TRUE(catalog.find("production_samples").has_value());
    }

    query::QueryTableCatalog reopened(file_system, "catalog-root");
    const auto discovered = reopened.find("production_samples");
    ASSERT_TRUE(discovered.has_value());
    EXPECT_EQ(discovered->row_count, 2);
    const auto batches = reopened.read(*discovered);
    ASSERT_TRUE(batches.ok()) << batches.status().ToString();
    ASSERT_EQ(batches->size(), 1U);
    EXPECT_EQ(batches->front()->num_rows(), 2);
    ASSERT_TRUE(reopened.drop("production_samples").ok());
    EXPECT_FALSE(reopened.find("production_samples").has_value());
}

TEST(QueryTableCatalogTest, SessionTableDoesNotOutliveItsCatalog)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    arrow::Int64Builder builder;
    ASSERT_TRUE(builder.Append(4).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(builder.Finish(&values).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", arrow::int64())}), 1, {values});
    {
        query::QueryTableCatalog catalog(file_system, "catalog-root");
        ASSERT_TRUE(catalog.create("recent", query::TableLifetime::Session, {batch}).ok());
        EXPECT_TRUE(catalog.find("recent").has_value());
    }
    query::QueryTableCatalog reopened(file_system, "catalog-root");
    EXPECT_FALSE(reopened.find("recent").has_value());
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

TEST(QueryFormatterTest, FormatsNativeMetadataCollectionsWithoutExpandingRows)
{
    auto tag_values = std::make_shared<arrow::StringBuilder>();
    arrow::ListBuilder tags_builder(arrow::default_memory_pool(), tag_values);
    auto attribute_keys = std::make_shared<arrow::StringBuilder>();
    auto attribute_values = std::make_shared<arrow::StringBuilder>();
    arrow::MapBuilder attributes_builder(arrow::default_memory_pool(), attribute_keys, attribute_values);
    ASSERT_TRUE(tags_builder.Append().ok());
    ASSERT_TRUE(tag_values->Append("sample").ok());
    ASSERT_TRUE(tag_values->Append("magnet").ok());
    ASSERT_TRUE(attributes_builder.Append().ok());
    ASSERT_TRUE(attribute_keys->Append("units").ok());
    ASSERT_TRUE(attribute_values->Append("A").ok());
    ASSERT_TRUE(attribute_keys->Append("namespace").ok());
    ASSERT_TRUE(attribute_values->Append("mldp_sample").ok());
    std::shared_ptr<arrow::Array> tags;
    std::shared_ptr<arrow::Array> attributes;
    ASSERT_TRUE(tags_builder.Finish(&tags).ok());
    ASSERT_TRUE(attributes_builder.Finish(&attributes).ok());
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("tags", tags->type()), arrow::field("attributes", attributes->type())}),
        1,
        {tags, attributes});
    const query::QueryExecutionResult result{.batches = {batch}};

    std::ostringstream json;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Json, json);
    EXPECT_EQ(json.str(), "{\"tags\":[\"sample\",\"magnet\"],\"attributes\":{\"units\":\"A\",\"namespace\":\"mldp_sample\"}}\n");

    std::ostringstream csv;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Csv, csv);
    EXPECT_EQ(csv.str(),
              "tags,attributes\n"
              "\"[\"\"sample\"\",\"\"magnet\"\"]\",\"{\"\"units\"\":\"\"A\"\",\"\"namespace\"\":\"\"mldp_sample\"\"}\"\n");

    std::ostringstream table;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Table, table);
    EXPECT_NE(table.str().find("sample"), std::string::npos);
    EXPECT_NE(table.str().find("magnet"), std::string::npos);
    EXPECT_NE(table.str().find("namespace=mldp_sample"), std::string::npos);
    EXPECT_NE(table.str().find("sample, magnet"), std::string::npos);
    EXPECT_NE(table.str().find("namespace=mldp_sample, units=A"), std::string::npos);

    std::ostringstream expanded;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Table, expanded, true);
    EXPECT_NE(expanded.str().find("-[ RECORD 1 ]"), std::string::npos);
    EXPECT_NE(expanded.str().find("  - sample"), std::string::npos);
    EXPECT_NE(expanded.str().find("  namespace: mldp_sample"), std::string::npos);
}

TEST(QueryFormatterTest, DisplaysActiveDenseUnionMembersWithoutArrowDiagnostics)
{
    const auto value_type = arrow::dense_union({arrow::field("string", arrow::utf8()), arrow::field("double", arrow::float64())});
    auto string_builder = std::make_shared<arrow::StringBuilder>();
    auto double_builder = std::make_shared<arrow::DoubleBuilder>();
    arrow::DenseUnionBuilder values_builder(
        arrow::default_memory_pool(), {string_builder, double_builder}, value_type);
    ASSERT_TRUE(values_builder.Append(0).ok());
    ASSERT_TRUE(string_builder->Append("sample").ok());
    ASSERT_TRUE(values_builder.Append(1).ok());
    ASSERT_TRUE(double_builder->Append(10.0).ok());
    ASSERT_TRUE(values_builder.AppendNull().ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(values_builder.Finish(&values).ok());
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("value", value_type)}), 3, {values});
    const query::QueryExecutionResult result{.batches = {batch}};

    std::ostringstream table;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Table, table);
    EXPECT_NE(table.str().find("sample"), std::string::npos);
    EXPECT_NE(table.str().find("10"), std::string::npos);
    EXPECT_EQ(table.str().find("union{"), std::string::npos);

    std::ostringstream expanded;
    cli::formatQueryResult(result, cli::QueryOutputFormat::Table, expanded, true);
    EXPECT_NE(expanded.str().find("value: sample"), std::string::npos);
    EXPECT_NE(expanded.str().find("value: 10"), std::string::npos);
    EXPECT_EQ(expanded.str().find("union{"), std::string::npos);
}

TEST(QueryFormatterTest, FitsTableValuesAndHeadersToExplicitViewport)
{
    arrow::StringBuilder first_builder;
    arrow::StringBuilder second_builder;
    ASSERT_TRUE(first_builder.Append("prefix-that-needs-truncation-suffix").ok());
    ASSERT_TRUE(second_builder.Append("another-long-value").ok());
    std::shared_ptr<arrow::Array> first;
    std::shared_ptr<arrow::Array> second;
    ASSERT_TRUE(first_builder.Finish(&first).ok());
    ASSERT_TRUE(second_builder.Finish(&second).ok());
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("first_very_long_column", arrow::utf8()), arrow::field("second_very_long_column", arrow::utf8())}),
        1,
        {first, second});
    const query::QueryExecutionResult result{.batches = {batch}};

    std::ostringstream output;
    cli::formatQueryResult(result,
                           cli::QueryOutputFormat::Table,
                           output,
                           false,
                           cli::TableRenderOptions{.viewport_width = 25});

    const auto rendered = output.str();
    EXPECT_NE(rendered.find("firs...lumn"), std::string::npos);
    EXPECT_NE(rendered.find("pref...ffix"), std::string::npos);
    std::istringstream lines(rendered);
    std::string line;
    while (std::getline(lines, line))
    {
        EXPECT_LE(line.size(), 25U);
    }
}

TEST(QueryFormatterTest, UsesStackedLayoutWhenViewportCannotFitAllColumns)
{
    arrow::StringBuilder first_builder;
    arrow::StringBuilder second_builder;
    ASSERT_TRUE(first_builder.Append("value").ok());
    ASSERT_TRUE(second_builder.Append("value").ok());
    std::shared_ptr<arrow::Array> first;
    std::shared_ptr<arrow::Array> second;
    ASSERT_TRUE(first_builder.Finish(&first).ok());
    ASSERT_TRUE(second_builder.Finish(&second).ok());
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("first", arrow::utf8()), arrow::field("second", arrow::utf8())}), 1, {first, second});
    const query::QueryExecutionResult result{.batches = {batch}};

    std::ostringstream output;
    cli::formatQueryResult(result,
                           cli::QueryOutputFormat::Table,
                           output,
                           false,
                           cli::TableRenderOptions{.viewport_width = 3});

    EXPECT_NE(output.str().find("..."), std::string::npos);
    EXPECT_EQ(output.str().find(" | "), std::string::npos);
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

TEST(QuerySubcommandTest, PreparesNestedQueryablePoolsFromInlineConfiguration)
{
    cli::QuerySubcommandPreparer preparer;
    query::QueryableFactory::instance().reset();
    const auto config = config::loadMergedConfigSources({"queryable.mldp.mldp-pool.query-url=localhost:2",
                                                         "queryable.mldp.mldp-pool.min-conn=1",
                                                         "queryable.mldp.mldp-pool.max-conn=2",
                                                         "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=localhost:3",
                                                         "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1",
                                                         "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=2"});

    preparer.prepare(config);
    EXPECT_EQ(query::QueryableFactory::instance().registeredTables(),
              (std::set<std::string>{"mldp.active_configurations",
                                     "mldp.configuration",
                                     "mldp.configuration_activation",
                                     "mldp.pv_metadata",
                                     "mldp.pv_stats",
                                     "mldp.time_series"}));
    EXPECT_NE(query::QueryableFactory::instance().createByTable("mldp.configuration"), nullptr);
    EXPECT_NE(query::QueryableFactory::instance().createByTable("mldp.time_series"), nullptr);
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, ReportsRegisteredClientInitializationFailureInsteadOfUnknownTable)
{
    cli::QuerySubcommandPreparer preparer;
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "queryable:\n" "  mldp:\n" "    query-url: localhost:2\n" "    min-conn: 1\n" "    max-conn: 1\n" "  mldp-pv-metadata:\n" "    annotation-url: localhost:3\n" "    min-conn: 1\n" "    max-conn: 1\n");
    preparer.prepare(config);

    const auto statement = query::parseQuery("SELECT * FROM mldp.configuration");
    EXPECT_NO_THROW(query::planner::bindSelect(std::get<query::SelectStatement>(statement)));
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, ReportsAnnotationClientConfigurationErrorsWithoutHidingTheTable)
{
    cli::QuerySubcommandPreparer preparer;
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "queryable:\n" "  mldp-pv-metadata:\n" "    min-conn: 1\n" "    max-conn: 1\n");
    preparer.prepare(config);

    const auto statement = query::parseQuery("SELECT * FROM mldp.configuration");
    try
    {
        (void)query::planner::bindSelect(std::get<query::SelectStatement>(statement));
        FAIL() << "Expected annotation client construction to fail";
    }
    catch (const query::plan::PlannerException& ex)
    {
        const auto message = query::plan::plannerErrorWhat(ex.error());
        EXPECT_NE(message.find("Failed to initialize query client"), std::string::npos);
        EXPECT_NE(message.find("annotation-url"), std::string::npos);
        EXPECT_EQ(message.find("Unknown table"), std::string::npos);
    }
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, OneShotReportsPlanningOrExecutionErrors)
{
    char arg0[] = "query";
    char arg1[] = "select * from mldp.pv_stats";
    char* argv[] = {arg0, arg1};
    cli::QuerySubcommand querySubcommand;
    std::ostringstream output;
    std::ostringstream error;
    std::istringstream input;
    EXPECT_EQ(querySubcommand.run(2, argv, {}, input, output, error), 1);
    EXPECT_TRUE(error.str().find("BindError") != std::string::npos ||
                error.str().find("Query error:") != std::string::npos);
}

TEST(QuerySubcommandTest, OneShotReportsParseErrors)
{
    char arg0[] = "query";
    char arg1[] = "select from mldp.pv_stats";
    char* argv[] = {arg0, arg1};
    cli::QuerySubcommand querySubcommand;
    std::ostringstream output;
    std::ostringstream error;
    std::istringstream input;
    EXPECT_EQ(querySubcommand.run(2, argv, {}, input, output, error), 1);
    EXPECT_NE(error.str().find("Parse error at "), std::string::npos);
}

TEST(QuerySubcommandTest, ReplShowsHelpAndExitsOnQuit)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input(".help\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;

    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};
    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(output.str().find("mldp> "), std::string::npos);
    EXPECT_NE(output.str().find("Enter one SQL statement terminated by ';'."), std::string::npos);
    EXPECT_NE(output.str().find("Ctrl-L (clear screen)"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(QuerySubcommandTest, ReplFormatPersistsForFollowingStatements)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input(".format\n.format json\nSHOW TABLES;\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(output.str().find("Output format: table"), std::string::npos);
    EXPECT_NE(output.str().find("Output format: json"), std::string::npos);
    EXPECT_NE(output.str().find("{\"table_name\":\"mldp.time_series\"}"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(QuerySubcommandTest, ReplTableFitCanBeChangedAndReported)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input(".table-fit\n.table-fit on\n.table-fit\n.table-fit invalid\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(output.str().find("Table fit: off"), std::string::npos);
    EXPECT_NE(output.str().find("Table fit: on"), std::string::npos);
    EXPECT_NE(error.str().find(".table-fit accepts on or off"), std::string::npos);
}

TEST(QuerySubcommandTest, ReplRejectsInvalidFormatWithoutChangingTheSession)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input(".format json\n.format invalid\n.format\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(error.str().find("Invalid --format value 'invalid'"), std::string::npos);
    EXPECT_NE(output.str().find("Output format: json"), std::string::npos);
}

TEST(QueryCompletionTest, CompletesCommandsKeywordsTablesAndColumns)
{
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "query-url: localhost:2\nmin-conn: 1\nmax-conn: 1\n");
    query::QueryableFactory::instance().prepare<query::impl::mldp::MLDPQueryClient>(config);

    EXPECT_EQ(cli::detail::replCompletions("sel"), std::vector<std::string>({"SELECT"}));
    EXPECT_EQ(cli::detail::replCompletions(".for"), std::vector<std::string>({".format"}));
    EXPECT_EQ(cli::detail::replCompletions(".format j"), std::vector<std::string>({"json"}));
    const auto tables = cli::detail::replCompletions("SELECT * FROM mldp.t");
    EXPECT_NE(std::find(tables.begin(), tables.end(), "mldp.time_series"), tables.end());
    const auto columns = cli::detail::replCompletions("SELECT ts.p FROM mldp.time_series AS ts WHERE ts.p");
    EXPECT_NE(std::find(columns.begin(), columns.end(), "ts.pv"), columns.end());
    EXPECT_TRUE(cli::detail::replCompletions("SELECT 'sel").empty());
    EXPECT_EQ(cli::detail::replCompletionContextLength("SELECT ts.p"), 4);

    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, ReplReportsIncompleteStatementAtEndOfInput)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input("SELECT *\nFROM mldp.pv_stats\n");
    std::ostringstream output;
    std::ostringstream error;

    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};
    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(error.str().find("incomplete SQL statement discarded"), std::string::npos);
}

TEST(QuerySubcommandTest, ReplRunsSubmittedStatementsAndRecoversFromParseErrors)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input("select from mldp.pv_stats;\nSHOW TABLES;\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(error.str().find("Parse error at "), std::string::npos);
    EXPECT_NE(output.str().find("mldp.time_series"), std::string::npos);
}

TEST(QuerySubcommandTest, ReplRecognisesSemicolonsOnlyOutsideQuotedStrings)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input("SELECT *\nFROM mldp.pv_stats\nWHERE pv = 'A;B';\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_EQ(error.str().find("Unexpected character ';'"), std::string::npos);
    EXPECT_NE(output.str().find("...> "), std::string::npos);
}

TEST(QuerySubcommandTest, ReplClearAndUnknownCommandKeepSessionUsable)
{
    char                           arg0[] = "query";
    char*                          argv[] = {arg0};
    cli::QuerySubcommand           querySubcommand;
    std::istringstream             input("SHOW TABLES;\nhistory\nSELECT *\n.clear\n.unknown\nSHOW TABLES;\n.quit\n");
    std::ostringstream             output;
    std::ostringstream             error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(error.str().find("unknown REPL command '.unknown'"), std::string::npos);
    EXPECT_EQ(error.str().find("Expected valid query syntax"), std::string::npos);
    EXPECT_NE(output.str().find("SHOW TABLES"), std::string::npos);
    EXPECT_NE(output.str().find("Buffered statement cleared."), std::string::npos);
    EXPECT_NE(output.str().find("mldp.time_series"), std::string::npos);
}

TEST(QuerySubcommandTest, ReplPlainStreamFallbackDoesNotWriteInteractiveHistory)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input(".help\n.quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.query-url=localhost:2",
        "queryable.mldp.min-conn=1",
        "queryable.mldp.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_NE(output.str().find("mldp> "), std::string::npos);
    EXPECT_TRUE(error.str().empty());
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
    std::istringstream input;
    EXPECT_EQ(querySubcommand.run(4, argv, {}, input, output, error), 1);
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
    std::istringstream input;
    EXPECT_EQ(querySubcommand.run(2, argv, {}, input, output, error), 1);
    EXPECT_NE(error.str().find("Missing 'queryable' configuration"), std::string::npos);
}

} // namespace
