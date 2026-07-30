//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/QuerySubcommand.h>
#include <query/ConsoleFooter.h>
#include <query/QueryCancellation.h>
#include <query/QueryExecutor.h>
#include <query/executor/ExecutorUtils.h>
#include <query/executor/ScanExecutionHelpers.h>
#include <query/QueryFormatter.h>
#include <query/QueryPlanner.h>
#include <query/QueryProgress.h>
#include <query/QueryResult.h>
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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

using namespace mldp_pvxs_driver;

namespace {

class ReplFakeQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;

    explicit ReplFakeQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {{"pv", query::ColumnType::STRING, false, true, {}, {}, "PV"},
                {"value", query::ColumnType::INT, false, true, {}, {}, "Value"}};
    }

    query::QueryResult execute(std::string_view,
                               const std::vector<query::Predicate>&,
                               const std::set<std::string>&,
                               const query::ExecutionContext&,
                               std::string_view = {}) override
    {
        arrow::StringBuilder pv_builder;
        arrow::Int64Builder value_builder;
        EXPECT_TRUE(pv_builder.Append("MAGNET:01").ok());
        EXPECT_TRUE(value_builder.Append(42).ok());
        std::shared_ptr<arrow::Array> pv;
        std::shared_ptr<arrow::Array> value;
        EXPECT_TRUE(pv_builder.Finish(&pv).ok());
        EXPECT_TRUE(value_builder.Finish(&value).ok());
        return {.batch = arrow::RecordBatch::Make(
                    arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("value", arrow::int64())}), 1, {pv, value})};
    }
};

class ObservingRecordBatchStream final : public query::IRecordBatchStream
{
public:
    ObservingRecordBatchStream(std::vector<std::shared_ptr<arrow::RecordBatch>> batches, std::ostringstream& output)
        : batches_(std::move(batches)), output_(output)
    {
    }

    std::shared_ptr<arrow::RecordBatch> next() override
    {
        if (index_ == 1)
        {
            first_batch_visible_before_second_pull = output_.str().find("\"value\":1") != std::string::npos;
        }
        return index_ < batches_.size() ? batches_[index_++] : nullptr;
    }

    bool first_batch_visible_before_second_pull{false};

private:
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    std::ostringstream& output_;
    std::size_t index_{0};
};

class EmptyRecordBatchStream final : public query::IRecordBatchStream
{
public:
    std::shared_ptr<arrow::RecordBatch> next() override { return nullptr; }
};

class ContinuationPageQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;
    inline static uint64_t stream_creations{0};
    inline static uint64_t next_calls{0};
    inline static std::vector<std::vector<int64_t>> batches{{1, 2, 3}, {4, 5}};

    explicit ContinuationPageQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override { return kVirtualTables; }

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "Time"},
                {"value", query::ColumnType::INT, false, true, {}, {}, "Value"}};
    }

    query::QueryResult execute(std::string_view,
                               const std::vector<query::Predicate>&,
                               const std::set<std::string>&,
                               const query::ExecutionContext&,
                               std::string_view = {}) override
    {
        throw std::runtime_error("ContinuationPageQueryable requires executeStream");
    }

    query::IRecordBatchStreamUPtr executeStream(std::string_view,
                                                 const std::vector<query::Predicate>&,
                                                 const std::set<std::string>&,
                                                 const query::ExecutionContext&,
                                                 std::string_view = {}) override
    {
        ++stream_creations;
        class Stream final : public query::IRecordBatchStream
        {
        public:
            explicit Stream(std::vector<std::vector<int64_t>> batches)
                : batches_(std::move(batches))
            {
            }

            std::shared_ptr<arrow::RecordBatch> next() override
            {
                ++ContinuationPageQueryable::next_calls;
                if (index_ >= batches_.size()) return nullptr;
                arrow::StringBuilder pv_builder;
                arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                arrow::Int64Builder value_builder;
                const auto& values = batches_[index_++];
                for (const auto value : values)
                {
                    if (!pv_builder.Append("CONT:PV").ok() ||
                        !time_builder.Append(value * 1'000'000'000LL).ok() ||
                        !value_builder.Append(value).ok())
                        throw std::runtime_error("Failed to build continuation test batch");
                }
                std::shared_ptr<arrow::Array> pv;
                std::shared_ptr<arrow::Array> time;
                std::shared_ptr<arrow::Array> value;
                if (!pv_builder.Finish(&pv).ok() || !time_builder.Finish(&time).ok() || !value_builder.Finish(&value).ok())
                    throw std::runtime_error("Failed to finish continuation test batch");
                return arrow::RecordBatch::Make(
                    arrow::schema({arrow::field("pv", pv->type()), arrow::field("time", time->type()), arrow::field("value", value->type())}),
                    pv->length(), {pv, time, value});
            }

        private:
            std::vector<std::vector<int64_t>> batches_;
            std::size_t index_{0};
        };
        return std::make_unique<Stream>(batches);
    }
};

class SustainedWindowQueryable final : public query::IQueryable
{
public:
    struct Request { std::string pv; int64_t begin; int64_t end; };
    static const std::set<std::string_view> kVirtualTables;
    inline static std::vector<Request> requests;
    inline static uint64_t stream_creations{0};
    inline static uint64_t next_calls{0};

    explicit SustainedWindowQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr) {}
    std::set<std::string_view> virtualTables() const override { return kVirtualTables; }
    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "Time"},
                {"value", query::ColumnType::INT, false, true, {}, {}, "Value"}};
    }
    query::QueryResult execute(std::string_view, const std::vector<query::Predicate>&, const std::set<std::string>&,
                               const query::ExecutionContext&, std::string_view = {}) override
    {
        throw std::runtime_error("SustainedWindowQueryable requires executeStream");
    }
    query::IRecordBatchStreamUPtr executeStream(std::string_view, const std::vector<query::Predicate>& predicates,
                                                const std::set<std::string>&, const query::ExecutionContext&,
                                                std::string_view = {}) override
    {
        std::string pv;
        int64_t begin = 0;
        int64_t end = 0;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "pv") pv = std::get<std::string>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::GTE) begin = std::get<int64_t>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::LTE) end = std::get<int64_t>(predicate.values.front());
        }
        requests.push_back({pv, begin, end});
        ++stream_creations;
        class Stream final : public query::IRecordBatchStream
        {
        public:
            Stream(std::string pv, const int64_t begin, const int64_t end) : pv_(std::move(pv)), begin_(begin), end_(end) {}
            std::shared_ptr<arrow::RecordBatch> next() override
            {
                ++SustainedWindowQueryable::next_calls;
                if (index_ == 2) return nullptr;
                const auto timestamp = index_++ == 0 ? (begin_ + 1) : end_; // inclusive shard end exercises local filtering
                const auto pv_number = pv_.back() - '0';
                arrow::StringBuilder pv;
                arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                arrow::Int64Builder value;
                if (!pv.Append(pv_).ok() || !time.Append(timestamp * 1'000'000'000LL).ok() || !value.Append(pv_number * 100 + timestamp).ok())
                    throw std::runtime_error("Failed to build sustained window batch");
                std::shared_ptr<arrow::Array> pv_array;
                std::shared_ptr<arrow::Array> time_array;
                std::shared_ptr<arrow::Array> value_array;
                if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
                    throw std::runtime_error("Failed to finish sustained window batch");
                return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}),
                                                1, {pv_array, time_array, value_array});
            }
        private:
            std::string pv_;
            int64_t begin_;
            int64_t end_;
            uint64_t index_{0};
        };
        return std::make_unique<Stream>(std::move(pv), begin, end);
    }
};

const std::set<std::string_view> ReplFakeQueryable::kVirtualTables = {"fake.samples"};
const std::set<std::string_view> ContinuationPageQueryable::kVirtualTables = {"mldp.time_series"};
const std::set<std::string_view> SustainedWindowQueryable::kVirtualTables = {"mldp.time_series"};

TEST(ConsoleFooterTest, RendersFixedWidthAsciiStatusWithPriority)
{
    cli::FooterRenderer renderer;
    cli::ConsoleStatus status;
    status.completed_stats = query::QueryStats{
        .elapsed = std::chrono::milliseconds(17),
        .rows_returned = 42,
        .rpc_calls = 3,
        .bytes_spilled = 2ULL * 1024ULL * 1024ULL,
        .peak_memory_bytes = 4ULL * 1024ULL * 1024ULL};

    const auto wide = renderer.render(status, 80);
    EXPECT_EQ(wide.size(), 80U);
    EXPECT_NE(wide.find("Query: ready | 42 rows | 17 ms | 3 RPCs | spill 2 MiB | peak 4 MiB"), std::string::npos);

    status.query_running = true;
    status.progress = query::QueryProgressSnapshot{.phase = query::QueryProgressPhase::BackendRpc,
                                                    .elapsed = std::chrono::milliseconds{12'000},
                                                    .table_name = "mldp.time_series_table",
                                                    .detail = "window",
                                                    .rpc_calls_started = 2,
                                                    .rpc_calls_completed = 1};
    const auto narrow = renderer.render(status, 24);
    EXPECT_EQ(narrow.size(), 24U);
    EXPECT_NE(narrow.find("Running: backend RPC"), std::string::npos);
    EXPECT_EQ(narrow.find("spill"), std::string::npos);
    EXPECT_TRUE(std::all_of(narrow.begin(), narrow.end(), [](const unsigned char character) { return character < 128; }));

    const auto tiny = renderer.render(status, 5);
    EXPECT_EQ(tiny, "Runni");

    const auto detailed = renderer.render(status, 120);
    EXPECT_NE(detailed.find("0m 12s"), std::string::npos);
    EXPECT_NE(detailed.find("mldp.time_series_table"), std::string::npos);
    EXPECT_NE(detailed.find("1/2 RPCs"), std::string::npos);

    status.progress = query::QueryProgressSnapshot{
        .phase = query::QueryProgressPhase::Executing,
        .elapsed = std::chrono::milliseconds{12'000},
        .table_name = "mldp.time_series_table",
        .operation = "wide pivot",
        .cursor_next_requests = 3,
        .cursor_responses = 4,
        .result_page = 2,
        .window_index = 1,
        .slice_index = 3,
        .series_shard_index = 2,
        .active_parallel_shards = 2,
        .parallel_shard_limit = 4};
    const auto pagination = renderer.render(status, 200);
    EXPECT_NE(pagination.find("wide pivot"), std::string::npos);
    EXPECT_NE(pagination.find("result page 2"), std::string::npos);
    EXPECT_NE(pagination.find("window 1, slice 3, series shard 2"), std::string::npos);
    EXPECT_NE(pagination.find("shards 2/4"), std::string::npos);
    EXPECT_NE(pagination.find("cursor 4, next 3"), std::string::npos);
}

TEST(QueryCancellationTest, RequestsCancellationOnceAndInvokesLateRegistration)
{
    query::QueryCancellation cancellation;
    int calls = 0;
    auto registration = cancellation.onCancel([&] { ++calls; });
    cancellation.requestCancel();
    cancellation.requestCancel();
    EXPECT_EQ(calls, 1);
    EXPECT_THROW(cancellation.throwIfCancelled(), query::QueryCancelled);
    cancellation.onCancel([&] { ++calls; });
    EXPECT_EQ(calls, 2);
}

TEST(QueryFormatterTest, StopsBeforeWritingWhenCancelled)
{
    arrow::Int64Builder values_builder;
    ASSERT_TRUE(values_builder.Append(42).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(values_builder.Finish(&values).ok());
    const query::QueryExecutionResult result{
        .batches = {arrow::RecordBatch::Make(arrow::schema({arrow::field("value", arrow::int64())}), 1, {values})}};
    auto cancellation = std::make_shared<query::QueryCancellation>();
    cancellation->requestCancel();
    std::ostringstream output;

    EXPECT_THROW(cli::formatQueryResult(result, cli::QueryOutputFormat::Json, output, false, {}, cancellation),
                 query::QueryCancelled);
    EXPECT_TRUE(output.str().empty());
}

TEST(QueryFormatterTest, WritesCompletedStreamBatchBeforePullingTheNext)
{
    const auto schema = arrow::schema({arrow::field("value", arrow::int64())});
    arrow::Int64Builder first_builder;
    arrow::Int64Builder second_builder;
    ASSERT_TRUE(first_builder.Append(1).ok());
    ASSERT_TRUE(second_builder.Append(2).ok());
    std::shared_ptr<arrow::Array> first;
    std::shared_ptr<arrow::Array> second;
    ASSERT_TRUE(first_builder.Finish(&first).ok());
    ASSERT_TRUE(second_builder.Finish(&second).ok());
    std::ostringstream output;
    ObservingRecordBatchStream stream({arrow::RecordBatch::Make(schema, 1, {first}),
                                       arrow::RecordBatch::Make(schema, 1, {second})},
                                      output);

    cli::formatQueryStream(stream, cli::QueryOutputFormat::Json, output);

    EXPECT_TRUE(stream.first_batch_visible_before_second_pull);
    EXPECT_NE(output.str().find("\"value\":2"), std::string::npos);
}

TEST(QueryProgressTest, TracksWindowShardsAndFormattedOutput)
{
    query::QueryableFactory::instance().reset();
    SustainedWindowQueryable::requests.clear();
    SustainedWindowQueryable::stream_creations = 0;
    SustainedWindowQueryable::next_calls = 0;
    query::QueryableFactory::instance().prepare<SustainedWindowQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner planner;
    query::QueryExecutor executor;
    auto progress = std::make_shared<query::QueryProgressTracker>();
    auto streamed = executor.executeStream(
        planner.plan(query::parseQuery(
            "SELECT pv, time, value FROM mldp.time_series WHERE pv IN ('PV1', 'PV2', 'PV3') "
            "AND window IN (0, 10; slice 5s, series_per_shard 1)")),
        {.pool = arrow::default_memory_pool(), .progress = progress});

    std::ostringstream output;
    cli::formatQueryStream(*streamed.stream, cli::QueryOutputFormat::Json, output, false, {}, nullptr, progress);

    const auto snapshot = progress->snapshot();
    EXPECT_EQ(snapshot.table_name, "mldp.time_series");
    EXPECT_EQ(snapshot.operation, "windowed MLDP scan");
    EXPECT_EQ(snapshot.window_index, 1U);
    EXPECT_EQ(snapshot.slice_index, 2U);
    EXPECT_EQ(snapshot.series_shard_index, 3U);
    EXPECT_EQ(snapshot.series_in_shard, 1U);
    EXPECT_EQ(snapshot.completed_shards, 6U);
    EXPECT_EQ(snapshot.output_batches, 12U);
    EXPECT_EQ(snapshot.rows_returned, 9U);
    EXPECT_EQ(SustainedWindowQueryable::stream_creations, 6U);
    EXPECT_NE(output.str().find("\"value\":310"), std::string::npos);

    query::QueryableFactory::instance().reset();
}

TEST(QueryContinuationRegistryTest, RotatesTokensAndRejectsMismatchedQueries)
{
    cli::QueryContinuationRegistry registry;
    const auto token = registry.store(cli::QueryContinuationRegistry::Entry{
        .fingerprint = "SELECT value FROM fake.samples LIMIT 1",
        .stream = std::make_unique<EmptyRecordBatchStream>(),
        .stats = std::make_shared<query::QueryStats>(),
        .cancellation = std::make_shared<query::QueryCancellation>()});

    EXPECT_TRUE(token.starts_with("p9:"));
    EXPECT_THROW(registry.take(token, "SELECT value FROM another.samples LIMIT 1"), std::runtime_error);
    auto entry = registry.take(token, "SELECT value FROM fake.samples LIMIT 1");
    ASSERT_NE(entry.stream, nullptr);
    EXPECT_THROW(registry.take(token, "SELECT value FROM fake.samples LIMIT 1"), std::runtime_error);
}

TEST(QueryContinuationRegistryTest, CancelsExpiredAndClearedContinuationWork)
{
    const auto make_entry = [] {
        auto cancellation = std::make_shared<query::QueryCancellation>();
        return std::pair{cancellation, cli::QueryContinuationRegistry::Entry{
                                           .fingerprint = "SELECT value FROM fake.samples LIMIT 1",
                                           .stream = std::make_unique<EmptyRecordBatchStream>(),
                                           .stats = std::make_shared<query::QueryStats>(),
                                           .cancellation = cancellation}};
    };

    cli::QueryContinuationRegistry expiring_registry(std::chrono::steady_clock::duration::zero());
    auto [expired_cancellation, expired_entry] = make_entry();
    const auto expired_token = expiring_registry.store(std::move(expired_entry));
    expiring_registry.cleanupExpired();
    EXPECT_TRUE(expired_cancellation->cancelled());
    EXPECT_THROW(expiring_registry.take(expired_token, "SELECT value FROM fake.samples LIMIT 1"), std::runtime_error);

    cli::QueryContinuationRegistry clearing_registry;
    auto [cleared_cancellation, cleared_entry] = make_entry();
    clearing_registry.store(std::move(cleared_entry));
    clearing_registry.clear();
    EXPECT_TRUE(cleared_cancellation->cancelled());
}

TEST(QueryRunnerTest, RetainsStreamingRowsAcrossRotatingInteractivePageTokens)
{
    query::QueryableFactory::instance().reset();
    ContinuationPageQueryable::stream_creations = 0;
    ContinuationPageQueryable::next_calls = 0;
    ContinuationPageQueryable::batches = {{1, 2, 3}, {4, 5}};
    query::QueryableFactory::instance().prepare<ContinuationPageQueryable>(config::Config::configFromYamlString("{}"));
    cli::QueryRunner runner;
    cli::QueryContinuationRegistry continuations;
    cli::QueryCliOptions options{.format = cli::QueryOutputFormat::Json, .no_stats = true};
    const std::string first_sql = "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'CONT:PV' LIMIT 2";
    std::ostringstream first_output;

    ASSERT_EQ(runner.run(options, first_sql, first_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
    EXPECT_NE(first_output.str().find("\"value\":1"), std::string::npos);
    EXPECT_NE(first_output.str().find("\"value\":2"), std::string::npos);
    EXPECT_EQ(first_output.str().find("\"value\":3"), std::string::npos);
    ASSERT_EQ(ContinuationPageQueryable::stream_creations, 1U);
    ASSERT_EQ(ContinuationPageQueryable::next_calls, 1U);

    const auto first_token_marker = first_output.str().find("p9:");
    ASSERT_NE(first_token_marker, std::string::npos);
    const auto first_token = first_output.str().substr(first_token_marker, first_output.str().find('\n', first_token_marker) - first_token_marker);
    const std::string resumed_sql = first_sql + " PAGE TOKEN '" + first_token + "'";
    std::ostringstream second_output;

    ASSERT_EQ(runner.run(options, resumed_sql, second_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
    EXPECT_NE(second_output.str().find("\"value\":3"), std::string::npos);
    EXPECT_NE(second_output.str().find("\"value\":4"), std::string::npos);
    EXPECT_EQ(second_output.str().find("\"value\":5"), std::string::npos);
    EXPECT_EQ(ContinuationPageQueryable::stream_creations, 1U);
    EXPECT_EQ(ContinuationPageQueryable::next_calls, 2U);

    const auto second_token_marker = second_output.str().find("p9:");
    ASSERT_NE(second_token_marker, std::string::npos);
    const auto second_token = second_output.str().substr(second_token_marker, second_output.str().find('\n', second_token_marker) - second_token_marker);
    EXPECT_NE(second_token, first_token);
    EXPECT_THROW(runner.run(options, resumed_sql, second_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), std::runtime_error);
    const std::string mismatched_sql = "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'OTHER:PV' LIMIT 2 PAGE TOKEN '" + second_token + "'";
    EXPECT_THROW(runner.run(options, mismatched_sql, second_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations),
                 std::runtime_error);

    std::ostringstream final_output;
    const std::string final_sql = first_sql + " PAGE TOKEN '" + second_token + "'";
    ASSERT_EQ(runner.run(options, final_sql, final_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
    EXPECT_NE(final_output.str().find("\"value\":5"), std::string::npos);
    EXPECT_EQ(final_output.str().find("p9:"), std::string::npos);
    EXPECT_EQ(ContinuationPageQueryable::stream_creations, 1U);
    EXPECT_EQ(ContinuationPageQueryable::next_calls, 3U);

    EXPECT_THROW(runner.run(options,
                            "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'CONT:PV' LIMIT 2 PAGE TOKEN 'p9:other-process'",
                            second_output),
                 std::runtime_error);
    query::QueryableFactory::instance().reset();
}

TEST(QueryRunnerTest, HandlesExactBackendPageBoundariesAndEmptyBatches)
{
    query::QueryableFactory::instance().reset();
    ContinuationPageQueryable::stream_creations = 0;
    ContinuationPageQueryable::next_calls = 0;
    query::QueryableFactory::instance().prepare<ContinuationPageQueryable>(config::Config::configFromYamlString("{}"));
    cli::QueryRunner runner;
    cli::QueryContinuationRegistry continuations;
    const cli::QueryCliOptions options{.format = cli::QueryOutputFormat::Json, .no_stats = true};
    const std::string sql = "SELECT pv, time, value FROM mldp.time_series WHERE pv = 'CONT:PV' LIMIT 2";

    ContinuationPageQueryable::batches = {{10, 11}, {12, 13}};
    std::ostringstream first_output;
    ASSERT_EQ(runner.run(options, sql, first_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
    EXPECT_NE(first_output.str().find("\"value\":10"), std::string::npos);
    EXPECT_NE(first_output.str().find("\"value\":11"), std::string::npos);
    EXPECT_EQ(ContinuationPageQueryable::next_calls, 1U);
    const auto first_marker = first_output.str().find("p9:");
    ASSERT_NE(first_marker, std::string::npos);
    const auto first_token = first_output.str().substr(first_marker, first_output.str().find('\n', first_marker) - first_marker);

    std::ostringstream second_output;
    ASSERT_EQ(runner.run(options, sql + " PAGE TOKEN '" + first_token + "'", second_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
    EXPECT_NE(second_output.str().find("\"value\":12"), std::string::npos);
    EXPECT_NE(second_output.str().find("\"value\":13"), std::string::npos);
    EXPECT_EQ(ContinuationPageQueryable::stream_creations, 1U);
    EXPECT_EQ(ContinuationPageQueryable::next_calls, 2U);
    const auto second_marker = second_output.str().find("p9:");
    ASSERT_NE(second_marker, std::string::npos);
    const auto second_token = second_output.str().substr(second_marker, second_output.str().find('\n', second_marker) - second_marker);

    std::ostringstream eof_output;
    ASSERT_EQ(runner.run(options, sql + " PAGE TOKEN '" + second_token + "'", eof_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
    EXPECT_TRUE(eof_output.str().empty());
    EXPECT_EQ(ContinuationPageQueryable::next_calls, 3U);

    ContinuationPageQueryable::batches = {{}, {21, 22}};
    ContinuationPageQueryable::next_calls = 0;
    cli::QueryContinuationRegistry empty_batch_continuations;
    std::ostringstream empty_batch_output;
    ASSERT_EQ(runner.run(options, sql, empty_batch_output, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &empty_batch_continuations), 0);
    EXPECT_NE(empty_batch_output.str().find("\"value\":21"), std::string::npos);
    EXPECT_NE(empty_batch_output.str().find("\"value\":22"), std::string::npos);
    EXPECT_EQ(ContinuationPageQueryable::next_calls, 2U);
    EXPECT_NE(empty_batch_output.str().find("p9:"), std::string::npos);
    query::QueryableFactory::instance().reset();
}

TEST(QueryRunnerTest, PagesSustainedMultiPvWindowStreamWithoutLossOrDuplication)
{
    query::QueryableFactory::instance().reset();
    SustainedWindowQueryable::requests.clear();
    SustainedWindowQueryable::stream_creations = 0;
    SustainedWindowQueryable::next_calls = 0;
    query::QueryableFactory::instance().prepare<SustainedWindowQueryable>(config::Config::configFromYamlString("{}"));
    cli::QueryRunner runner;
    cli::QueryContinuationRegistry continuations;
    const cli::QueryCliOptions options{.format = cli::QueryOutputFormat::Json, .no_stats = true};
    const std::string sql = "SELECT pv, time, value FROM mldp.time_series WHERE pv IN ('PV1', 'PV2', 'PV3') AND window IN (0, 10; slice 5s, series_per_shard 1) LIMIT 2";
    std::string token;
    std::string output;
    std::string final_page;
    for (;;)
    {
        std::ostringstream page;
        const auto page_sql = token.empty() ? sql : sql + " PAGE TOKEN '" + token + "'";
        ASSERT_EQ(runner.run(options, page_sql, page, nullptr, std::nullopt, false, nullptr, nullptr, nullptr, &continuations), 0);
        output += page.str();
        const auto marker = page.str().find("p9:");
        if (marker == std::string::npos)
        {
            final_page = page.str();
            break;
        }
        token = page.str().substr(marker, page.str().find('\n', marker) - marker);
    }

    const std::vector<SustainedWindowQueryable::Request> expected{
        {"PV1", 0, 5}, {"PV2", 0, 5}, {"PV3", 0, 5}, {"PV1", 5, 10}, {"PV2", 5, 10}, {"PV3", 5, 10}};
    ASSERT_EQ(SustainedWindowQueryable::requests.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        EXPECT_EQ(SustainedWindowQueryable::requests[index].pv, expected[index].pv);
        EXPECT_EQ(SustainedWindowQueryable::requests[index].begin, expected[index].begin);
        EXPECT_EQ(SustainedWindowQueryable::requests[index].end, expected[index].end);
    }
    EXPECT_EQ(SustainedWindowQueryable::stream_creations, expected.size());
    EXPECT_EQ(SustainedWindowQueryable::next_calls, expected.size() * 3U);
    EXPECT_EQ(final_page.find("p9:"), std::string::npos);
    for (const auto value : {101LL, 201LL, 301LL, 106LL, 110LL, 206LL, 210LL, 306LL, 310LL})
    {
        const auto needle = "\"value\":" + std::to_string(value);
        std::size_t matches = 0;
        for (std::size_t position = output.find(needle); position != std::string::npos; position = output.find(needle, position + needle.size())) ++matches;
        EXPECT_EQ(matches, 1U);
    }
    for (const auto duplicate_boundary : {105LL, 205LL, 305LL})
        EXPECT_EQ(output.find("\"value\":" + std::to_string(duplicate_boundary)), std::string::npos);
    query::QueryableFactory::instance().reset();
}

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

TEST(SpillManagerTest, IncrementalWriterRoundTripsEveryBatch)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    query::SpillManager manager(file_system, "spill");
    const auto schema = arrow::schema({arrow::field("value", arrow::int64())});
    arrow::Int64Builder first_builder;
    arrow::Int64Builder second_builder;
    ASSERT_TRUE(first_builder.Append(1).ok());
    ASSERT_TRUE(second_builder.Append(2).ok());
    std::shared_ptr<arrow::Array> first_values;
    std::shared_ptr<arrow::Array> second_values;
    ASSERT_TRUE(first_builder.Finish(&first_values).ok());
    ASSERT_TRUE(second_builder.Finish(&second_values).ok());
    auto writer_result = manager.openWriter("incremental", schema);
    ASSERT_TRUE(writer_result.ok()) << writer_result.status().ToString();
    auto writer = std::move(*writer_result);
    ASSERT_TRUE(writer.append(arrow::RecordBatch::Make(schema, 1, {first_values})).ok());
    ASSERT_TRUE(writer.append(arrow::RecordBatch::Make(schema, 1, {second_values})).ok());
    auto spill = writer.finish();
    ASSERT_TRUE(spill.ok()) << spill.status().ToString();
    EXPECT_EQ(spill->batch_count, 2);
    auto reader_result = manager.read(*spill);
    ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
    auto reader = std::move(*reader_result);
    auto first = reader.next();
    auto second = reader.next();
    ASSERT_TRUE(first.ok()) << first.status().ToString();
    ASSERT_TRUE(second.ok()) << second.status().ToString();
    EXPECT_EQ(std::static_pointer_cast<arrow::Int64Array>((*first)->column(0))->Value(0), 1);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int64Array>((*second)->column(0))->Value(0), 2);
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

TEST(SpillManagerTest, WriterDestructorDeletesAbandonedFile)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    query::SpillManager manager(file_system, "spill");
    const auto schema = arrow::schema({arrow::field("value", arrow::int64())});
    arrow::Int64Builder builder;
    ASSERT_TRUE(builder.Append(7).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(builder.Finish(&values).ok());
    std::string path;
    {
        auto writer_result = manager.openWriter("abandoned", schema);
        ASSERT_TRUE(writer_result.ok()) << writer_result.status().ToString();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer.append(arrow::RecordBatch::Make(schema, 1, {values})).ok());
        path = "spill/spill_abandoned_0.arrow";
    }
    const auto file_info = file_system->GetFileInfo(path);
    ASSERT_TRUE(file_info.ok()) << file_info.status().ToString();
    EXPECT_EQ(file_info->type(), arrow::fs::FileType::NotFound);
}

TEST(PivotExecutionTest, CancellationDuringSpillIngestionDeletesTemporaryFiles)
{
    class CancellingStream final : public query::IRecordBatchStream
    {
    public:
        CancellingStream(std::shared_ptr<query::QueryCancellation> cancellation,
                         std::shared_ptr<arrow::RecordBatch> batch)
            : cancellation_(std::move(cancellation)), batch_(std::move(batch))
        {
        }

        std::shared_ptr<arrow::RecordBatch> next() override
        {
            if (calls_++ == 0) return batch_;
            cancellation_->requestCancel();
            return nullptr;
        }

    private:
        std::shared_ptr<query::QueryCancellation> cancellation_;
        std::shared_ptr<arrow::RecordBatch> batch_;
        int calls_{0};
    };

    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto spill = std::make_shared<query::SpillManager>(file_system, "spill");
    auto cancellation = std::make_shared<query::QueryCancellation>();
    arrow::StringBuilder keys;
    arrow::TimestampBuilder times(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    arrow::Int64Builder values;
    ASSERT_TRUE(keys.Append("COLUMN").ok());
    ASSERT_TRUE(times.Append(1).ok());
    ASSERT_TRUE(values.Append(42).ok());
    std::shared_ptr<arrow::Array> key_array;
    std::shared_ptr<arrow::Array> time_array;
    std::shared_ptr<arrow::Array> value_array;
    ASSERT_TRUE(keys.Finish(&key_array).ok());
    ASSERT_TRUE(times.Finish(&time_array).ok());
    ASSERT_TRUE(values.Finish(&value_array).ok());
    CancellingStream stream(cancellation, arrow::RecordBatch::Make(
        arrow::schema({arrow::field("key", key_array->type()), arrow::field("row", time_array->type()), arrow::field("value", value_array->type())}),
        1, {key_array, time_array, value_array}));
    query::QueryStats stats;

    EXPECT_THROW((void)query::executor::pivotLongStreamWithSpill(
                     stream, "row", "key", "value", {"COLUMN"}, 4096,
                     {.pool = arrow::default_memory_pool(), .spill = spill, .cancellation = cancellation}, stats),
                 query::QueryCancelled);
    ASSERT_TRUE(spill->cleanup().ok());
    const auto file_info = file_system->GetFileInfo("spill/spill_wide-long_0.arrow");
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
    EXPECT_EQ(table.str().find("(1 row)"), std::string::npos);

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

TEST(QueryFormatterTest, FormatsQueryStatsFooterLine)
{
    query::QueryStats stats;
    stats.elapsed = std::chrono::milliseconds(1);
    stats.rows_from_backend = 10;
    stats.rows_returned = 10;
    stats.rpc_calls = 0;
    stats.bytes_spilled = 0;
    stats.materialized_bytes = 0;
    stats.materialized_files = 0;
    stats.peak_memory_bytes = 0;

    EXPECT_EQ(cli::queryStatsLine(stats),
              "-- 10 rows (10 from backend, 0 filtered) in 1ms | 0 RPC | 0 bytes spilled | "
              "0 bytes materialized in 0 file(s) | 0 MB peak");
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
    EXPECT_EQ(expanded.str().find("(1 row)"), std::string::npos);
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
                                     "mldp.time_series",
                                     "mldp.time_series_table"}));
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

TEST(QuerySubcommandTest, EnforcesTheSpecialTimeSeriesTableSelectStarContract)
{
    cli::QuerySubcommandPreparer preparer;
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "queryable:\n" "  mldp:\n" "    query-url: localhost:2\n" "    min-conn: 1\n" "    max-conn: 1\n");
    preparer.prepare(config);

    EXPECT_NO_THROW(query::planner::bindSelect(std::get<query::SelectStatement>(
        query::parseQuery("SELECT * FROM mldp.time_series_table WHERE pv = 'SYS:MAGNET:CURRENT'"))));

    for (const std::string_view sql : {
             "SELECT time FROM mldp.time_series_table WHERE pv = 'SYS:MAGNET:CURRENT'",
             "SELECT * FROM mldp.time_series_table WHERE pv = 'SYS:MAGNET:CURRENT' ORDER BY time",
             "SELECT * FROM mldp.time_series_table ts INNER JOIN mldp.pv_stats stats ON ts.time = stats.first_timestamp "
             "WHERE ts.pv = 'SYS:MAGNET:CURRENT' AND stats.pv = 'SYS:MAGNET:CURRENT'"})
    {
        try
        {
            (void)query::planner::bindSelect(std::get<query::SelectStatement>(query::parseQuery(sql)));
            FAIL() << "Expected special-table contract failure for " << sql;
        }
        catch (const query::plan::PlannerException& error)
        {
            const auto message = query::plan::plannerErrorWhat(error.error());
            EXPECT_NE(message.find("mldp.time_series_table"), std::string::npos);
        }
    }
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, PlansTimeSeriesPvAndWindowSubqueries)
{
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "queryable:\n"
        "  mldp:\n"
        "    query-url: localhost:2\n"
        "    min-conn: 1\n"
        "    max-conn: 1\n"
        "  mldp-pv-metadata:\n"
        "    annotation-url: localhost:3\n"
        "    min-conn: 1\n"
        "    max-conn: 1\n");
    cli::QuerySubcommandPreparer preparer;
    preparer.prepare(config);

    const auto plan = query::QueryPlanner{}.plan(query::parseQuery(
        "SELECT * FROM mldp.time_series_table "
        "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE tag = 'magnet') "
        "AND window IN (SELECT activation.time, activation.end_time "
        "FROM mldp.configuration_activation activation "
        "INNER JOIN mldp.configuration configuration ON activation.config_name = configuration.name "
        "WHERE configuration.category = 'beam_mode' AND activation.end_time IS NOT NULL)"));
    const auto plan_text = query::plan::physicalPlanToString(plan);
    EXPECT_NE(plan_text.find("PhysicalPivot(columns=0, batch_size=4096)"), std::string::npos);
    EXPECT_NE(plan_text.find("PhysicalTableScan(table=mldp.time_series"), std::string::npos);
    EXPECT_NE(plan_text.find("in_subqueries=1"), std::string::npos);
    EXPECT_NE(plan_text.find("window_subquery=true"), std::string::npos);

    const auto long_plan = query::QueryPlanner{}.plan(query::parseQuery(
        "SELECT pv, time, value FROM mldp.time_series "
        "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE tag = 'magnet') "
        "AND window IN (SELECT activation.time, activation.end_time "
        "FROM mldp.configuration_activation activation "
        "INNER JOIN mldp.configuration configuration ON activation.config_name = configuration.name "
        "WHERE configuration.category = 'beam_mode' AND activation.end_time IS NOT NULL)"));
    const auto long_plan_text = query::plan::physicalPlanToString(long_plan);
    EXPECT_NE(long_plan_text.find("PhysicalTableScan(table=mldp.time_series"), std::string::npos);
    EXPECT_NE(long_plan_text.find("in_subqueries=1"), std::string::npos);
    EXPECT_NE(long_plan_text.find("window_subquery=true"), std::string::npos);
    query::QueryableFactory::instance().reset();
}

TEST(QueryExecutorTest, ExtractsWindowEndpointsByPositionRatherThanName)
{
    arrow::TimestampBuilder start_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    arrow::TimestampBuilder end_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    ASSERT_TRUE(start_builder.Append(10).ok());
    ASSERT_TRUE(end_builder.Append(20).ok());
    std::shared_ptr<arrow::Array> start;
    std::shared_ptr<arrow::Array> end;
    ASSERT_TRUE(start_builder.Finish(&start).ok());
    ASSERT_TRUE(end_builder.Finish(&end).ok());

    const query::executor::RecordBatches batches{
        arrow::RecordBatch::Make(arrow::schema({arrow::field("activation_time", start->type()), arrow::field("activation_time_2s", end->type())}), 1, {start, end})};
    EXPECT_EQ(query::executor::extractNormalizedWindows(batches), (std::vector<std::pair<int64_t, int64_t>>{{10, 20}}));
}

TEST(QuerySubcommandTest, PlansNarrowTimeSeriesPvSubqueryWithMetadataJoin)
{
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "queryable:\n"
        "  mldp:\n"
        "    query-url: localhost:2\n"
        "    min-conn: 1\n"
        "    max-conn: 1\n"
        "  mldp-pv-metadata:\n"
        "    annotation-url: localhost:3\n"
        "    min-conn: 1\n"
        "    max-conn: 1\n");
    cli::QuerySubcommandPreparer preparer;
    preparer.prepare(config);

    const auto plan = query::QueryPlanner{}.plan(query::parseQuery(
        "SELECT ts.pv, ts.time, ts.value, m.description "
        "FROM mldp.time_series ts "
        "JOIN mldp.pv_metadata m ON ts.pv = m.pv "
        "WHERE ts.pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv PREFIX 'mldp_sample:MAGNET') "
        "AND ts.time >= NOW - 10m AND ts.time <= NOW"));
    const auto plan_text = query::plan::physicalPlanToString(plan);
    EXPECT_NE(plan_text.find("PhysicalTableScan(table=mldp.time_series"), std::string::npos);
    EXPECT_NE(plan_text.find("in_subqueries=1"), std::string::npos);
    query::QueryableFactory::instance().reset();
}

TEST(QuerySubcommandTest, DescribesAndPlansLiteralTimeSeriesWindows)
{
    query::QueryableFactory::instance().reset();
    const auto config = config::Config::configFromYamlString(
        "queryable:\n"
        "  mldp:\n"
        "    query-url: localhost:2\n"
        "    min-conn: 1\n"
        "    max-conn: 1\n");
    cli::QuerySubcommandPreparer preparer;
    preparer.prepare(config);

    query::QueryPlanner planner;
    const auto plan = planner.plan(query::parseQuery(
        "SELECT * FROM mldp.time_series_table WHERE pv = 'SYS:MAGNET:CURRENT' AND window IN (20, 10)"));
    const auto plan_text = query::plan::physicalPlanToString(plan);
    EXPECT_NE(plan_text.find("window_literal=true"), std::string::npos);
    const auto long_plan = planner.plan(query::parseQuery(
        "SELECT pv, time FROM mldp.time_series WHERE pv = 'SYS:MAGNET:CURRENT' AND window IN (20, 10)"));
    EXPECT_NE(query::plan::physicalPlanToString(long_plan).find("window_literal=true"), std::string::npos);

    query::QueryExecutor executor;
    const auto describe = executor.execute(
        planner.plan(query::parseQuery("DESCRIBE mldp.time_series_table")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(describe.batches.size(), 1U);
    const auto& batch = describe.batches.front();
    const auto name = std::static_pointer_cast<arrow::StringArray>(batch->column(0));
    const auto type = std::static_pointer_cast<arrow::StringArray>(batch->column(1));
    const auto output = std::static_pointer_cast<arrow::BooleanArray>(batch->column(3));
    const auto pushable = std::static_pointer_cast<arrow::StringArray>(batch->column(4));
    int64_t window_index = -1;
    for (int64_t index = 0; index < name->length(); ++index)
    {
        if (name->GetString(index) == "window")
        {
            window_index = index;
            break;
        }
    }
    ASSERT_GE(window_index, 0);
    EXPECT_EQ(type->GetString(window_index), "timestamp");
    EXPECT_FALSE(output->Value(window_index));
    EXPECT_EQ(pushable->GetString(window_index), "IN");

    const auto long_describe = executor.execute(
        planner.plan(query::parseQuery("DESCRIBE mldp.time_series")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(long_describe.batches.size(), 1U);
    const auto long_names = std::static_pointer_cast<arrow::StringArray>(long_describe.batches.front()->column(0));
    bool long_has_window = false;
    for (int64_t index = 0; index < long_names->length(); ++index)
    {
        long_has_window = long_names->GetString(index) == "window";
        if (long_has_window) break;
    }
    EXPECT_TRUE(long_has_window);

    for (const std::string_view sql : {
             "SELECT * FROM mldp.time_series_table WHERE pv = 'P' AND window IN (10)",
             "SELECT * FROM mldp.time_series_table WHERE pv = 'P' AND window IN (10, 20, 30)",
             "SELECT * FROM mldp.time_series_table WHERE pv = 'P' AND window IN ('start', 'end')",
             "SELECT * FROM mldp.time_series_table WHERE pv = 'P' AND window IN (10, 'end')",
             "SELECT pv FROM mldp.pv_stats WHERE pv = 'P' AND window IN (10, 20)",
             "SELECT * FROM mldp.time_series_table WHERE pv = 'P' AND window IN (10, 20) AND window IN (SELECT time, end_time FROM mldp.configuration_activation)"})
    {
        EXPECT_THROW((void)planner.plan(query::parseQuery(std::string(sql))), query::plan::PlannerException) << sql;
    }
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

TEST(QuerySubcommandTest, ReplMaterializesMultilineCreateTableForFollowingSelect)
{
    const auto catalog_directory = std::filesystem::temp_directory_path() /
                                   ("mldp-query-repl-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto catalog_directory_string = catalog_directory.string();
    char arg0[] = "query";
    char arg1[] = "--table-catalog-dir";
    char* argv[] = {arg0, arg1, catalog_directory_string.data()};
    query::QueryableFactory::instance().reset();
    cli::QuerySubcommand querySubcommand([](const config::Config&)
    {
        query::QueryableFactory::instance().prepare<ReplFakeQueryable>(config::Config::configFromYamlString("{}"));
    });
    std::istringstream input(
        "CREATE TEMP TABLE magnet_samples AS\n"
        "SELECT pv, value FROM fake.samples;\n"
        "SELECT * FROM magnet_samples;\n"
        ".quit\n");
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(querySubcommand.run(3, argv, {}, input, output, error), 0);
    EXPECT_TRUE(error.str().empty()) << error.str();
    EXPECT_NE(output.str().find("MAGNET:01"), std::string::npos);
    EXPECT_NE(output.str().find("42"), std::string::npos);
    std::error_code remove_error;
    std::filesystem::remove_all(catalog_directory, remove_error);
    EXPECT_FALSE(remove_error);
    query::QueryableFactory::instance().reset();
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
    EXPECT_NE(output.str().find("{\"table_name\":\"mldp.time_series\",\"type\":\"virtual\",\"location\":\"\"}"),
              std::string::npos);
    EXPECT_NE(output.str().find("-- "), std::string::npos);
    EXPECT_NE(output.str().find("rows ("), std::string::npos);
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

TEST(QueryCompletionTest, CompletesSessionAndPersistentCatalogTablesAndColumns)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    arrow::StringBuilder name_builder;
    arrow::Int64Builder value_builder;
    ASSERT_TRUE(name_builder.Append("sample").ok());
    ASSERT_TRUE(value_builder.Append(7).ok());
    std::shared_ptr<arrow::Array> name;
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(name_builder.Finish(&name).ok());
    ASSERT_TRUE(value_builder.Finish(&value).ok());
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("sample_name", arrow::utf8()), arrow::field("sample_value", arrow::int64())}),
        1,
        {name, value});
    ASSERT_TRUE(catalog->create("session_samples", query::TableLifetime::Session, {batch}).ok());
    ASSERT_TRUE(catalog->create("persistent_samples", query::TableLifetime::Persistent, {batch}).ok());

    for (const std::string_view sql : {"SELECT * FROM ses", "DROP TABLE ses"})
    {
        const auto candidates = cli::detail::replCompletions(sql, catalog);
        EXPECT_NE(std::find(candidates.begin(), candidates.end(), "session_samples"), candidates.end()) << sql;
    }
    const auto describe = cli::detail::replCompletions("DESC per", catalog);
    EXPECT_NE(std::find(describe.begin(), describe.end(), "persistent_samples"), describe.end());
    const auto persistent = cli::detail::replCompletions("SELECT * FROM session_samples JOIN per", catalog);
    EXPECT_NE(std::find(persistent.begin(), persistent.end(), "persistent_samples"), persistent.end());

    const auto qualified = cli::detail::replCompletions("SELECT s.sample_ FROM session_samples AS s WHERE s.sample_", catalog);
    EXPECT_NE(std::find(qualified.begin(), qualified.end(), "s.sample_name"), qualified.end());
    EXPECT_NE(std::find(qualified.begin(), qualified.end(), "s.sample_value"), qualified.end());
    const auto unqualified = cli::detail::replCompletions("SELECT sample_ FROM session_samples WHERE sample_", catalog);
    EXPECT_NE(std::find(unqualified.begin(), unqualified.end(), "sample_name"), unqualified.end());
    EXPECT_NE(std::find(unqualified.begin(), unqualified.end(), "sample_value"), unqualified.end());
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
    EXPECT_EQ(output.str().find("\033[K"), std::string::npos);
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

TEST(QuerySubcommandTest, ReplRecognisesWindowShardSeparatorInsideNestedParentheses)
{
    char arg0[] = "query";
    char* argv[] = {arg0};
    cli::QuerySubcommand querySubcommand;
    std::istringstream input(
        "SELECT *\n"
        "FROM mldp.time_series_table\n"
        "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE attributes.dname PREFIX 'USEG:UNDH' LIMIT 2)\n"
        "AND window IN (SELECT time, time + 30s FROM mldp.configuration_activation "
        "WHERE time >= NOW - 120d AND config_name = 'SPEAR User' LIMIT 1; slice 15s, series_per_shard 2);\n"
        ".quit\n");
    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> config_sources{
        "queryable.mldp.mldp-pool.query-url=localhost:2",
        "queryable.mldp.mldp-pool.min-conn=1",
        "queryable.mldp.mldp-pool.max-conn=1",
        "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=localhost:2",
        "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1",
        "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=1"};

    EXPECT_EQ(querySubcommand.run(1, argv, config_sources, input, output, error), 0);
    EXPECT_EQ(error.str().find("only one SQL statement may be submitted at a time"), std::string::npos);
    EXPECT_EQ(error.str().find("Parse error"), std::string::npos);
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
