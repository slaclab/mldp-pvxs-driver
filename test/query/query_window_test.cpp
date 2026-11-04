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
#include <query/QueryExecutor.h>
#include <query/QueryPlanner.h>
#include <query/QueryTableCatalog.h>
#include <query/SpillManager.h>
#include <query/WindowFunctionRegistry.h>
#include <query/executor/MaterializedRecordBatchStream.h>
#include <query/executor/WindowEvaluator.h>
#include <query/executor/WindowRecordBatchStream.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PlannerError.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/filesystem/mockfs.h>
#include <arrow/memory_pool.h>
#include <arrow/scalar.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

using Rows = std::vector<std::vector<std::string>>;

ExpressionPtr column(const std::string& name)
{
    return std::make_shared<Expression>(Expression{.value = QualifiedColumn{.qualifier = std::nullopt, .name = name, .path = {}}});
}

/** Every cell as text ("null" for nulls), rows sorted so the comparison ignores output order. */
Rows rowsOf(const RecordBatches& batches, const bool sort = true)
{
    Rows rows;
    for (const auto& batch : batches)
    {
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            std::vector<std::string> cells;
            for (int index = 0; index < batch->num_columns(); ++index)
            {
                const auto scalar = *batch->column(index)->GetScalar(row);
                cells.push_back(scalar->is_valid ? scalar->ToString() : "null");
            }
            rows.push_back(std::move(cells));
        }
    }
    if (sort) std::sort(rows.begin(), rows.end());
    return rows;
}

Rows sorted(Rows rows)
{
    std::sort(rows.begin(), rows.end());
    return rows;
}

/** Window test table `ws(pv, t, v, ts)`; A has a tie at t = 3 (null value first), B a null t. */
class WindowQueryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        file_system_ = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
        catalog_ = std::make_shared<QueryTableCatalog>(file_system_, "catalog");
        const std::vector<std::string>            pvs{"A", "A", "B", "A", "A", "A", "B", "B"};
        const std::vector<std::optional<int64_t>> times{1, 2, 1, 3, 3, 5, 2, std::nullopt};
        const std::vector<std::optional<int64_t>> values{10, 20, 5, std::nullopt, 40, 50, 7, 9};
        arrow::StringBuilder    pv;
        arrow::Int64Builder     t;
        arrow::Int64Builder     v;
        arrow::TimestampBuilder ts(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        for (std::size_t row = 0; row < pvs.size(); ++row)
        {
            ASSERT_TRUE(pv.Append(pvs[row]).ok());
            ASSERT_TRUE((times[row] ? t.Append(*times[row]) : t.AppendNull()).ok());
            ASSERT_TRUE((values[row] ? v.Append(*values[row]) : v.AppendNull()).ok());
            ASSERT_TRUE((times[row] ? ts.Append(*times[row] * 1'000'000'000LL) : ts.AppendNull()).ok());
        }
        std::shared_ptr<arrow::Array> pv_array, t_array, v_array, ts_array;
        ASSERT_TRUE(pv.Finish(&pv_array).ok());
        ASSERT_TRUE(t.Finish(&t_array).ok());
        ASSERT_TRUE(v.Finish(&v_array).ok());
        ASSERT_TRUE(ts.Finish(&ts_array).ok());
        const auto schema = arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("t", arrow::int64()), arrow::field("v", arrow::int64()),
                                           arrow::field("ts", ts_array->type())});
        // Partitions span both batches.
        const auto first = arrow::RecordBatch::Make(schema, 4, {pv_array->Slice(0, 4), t_array->Slice(0, 4), v_array->Slice(0, 4), ts_array->Slice(0, 4)});
        const auto second = arrow::RecordBatch::Make(schema, 4, {pv_array->Slice(4, 4), t_array->Slice(4, 4), v_array->Slice(4, 4), ts_array->Slice(4, 4)});
        ASSERT_TRUE(catalog_->create("ws", TableLifetime::Session, {first, second}).ok());
    }

    RecordBatches run(const std::string& sql) const
    {
        const ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog_};
        QueryPlanner           planner(catalog_);
        return QueryExecutor{}.execute(planner.plan(parseQuery(sql)), context).batches;
    }

    RecordBatches stream(const std::string& sql) const
    {
        const ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog_};
        QueryPlanner           planner(catalog_);
        auto                   result = QueryExecutor{}.executeStream(planner.plan(parseQuery(sql)), context);
        RecordBatches          batches;
        while (auto batch = result.stream->next()) batches.push_back(batch);
        return batches;
    }

    void expectBindError(const std::string& sql) const
    {
        QueryPlanner planner(catalog_);
        EXPECT_THROW((void)planner.plan(parseQuery(sql)), plan::PlannerException) << sql;
    }

    std::shared_ptr<arrow::fs::internal::MockFileSystem> file_system_;
    std::shared_ptr<QueryTableCatalog>                   catalog_;
};

} // namespace

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST(WindowParserTest, ParsesOverClausesNamedWindowsAndFrames)
{
    const auto statement = std::get<SelectStatement>(parseQuery(
        "SELECT LAG(value) OVER w AS prev, ROW_NUMBER() OVER (PARTITION BY pv ORDER BY time DESC) rn, "
        "AVG(value) OVER (w RANGE BETWEEN 5m PRECEDING AND CURRENT ROW), COUNT(*) OVER (), "
        "SUM(value) OVER (ORDER BY time ROWS 2 PRECEDING) "
        "FROM mldp.time_series WHERE pv = 'A' WINDOW w AS (PARTITION BY pv ORDER BY time), w2 AS (w) ORDER BY time"));
    ASSERT_EQ(statement.select_items.size(), 5U);
    ASSERT_EQ(statement.named_windows.size(), 2U);
    EXPECT_EQ(statement.named_windows[0].name, "w");
    EXPECT_EQ(statement.named_windows[0].spec.partition_by.size(), 1U);
    EXPECT_EQ(statement.named_windows[1].spec.base_name, std::optional<std::string>{"w"});
    EXPECT_EQ(statement.order_by.size(), 1U);

    const auto call = [&](const std::size_t index) { return std::get<FunctionCall>(statement.select_items[index].expression->value); };
    EXPECT_EQ(call(0).over->base_name, std::optional<std::string>{"w"});
    EXPECT_EQ(statement.select_items[0].alias, std::optional<std::string>{"prev"});
    EXPECT_TRUE(call(1).arguments.empty());
    EXPECT_EQ(call(1).over->order_by.front().direction, SortDirection::DESCENDING);
    EXPECT_EQ(statement.select_items[1].alias, std::optional<std::string>{"rn"});
    ASSERT_TRUE(call(2).over->frame.has_value());
    EXPECT_EQ(call(2).over->frame->unit, WindowFrameUnit::RANGE);
    EXPECT_EQ(call(2).over->frame->start.kind, WindowBoundKind::PRECEDING);
    EXPECT_EQ(call(2).over->frame->end.kind, WindowBoundKind::CURRENT_ROW);
    EXPECT_TRUE(call(3).star);
    EXPECT_FALSE(call(3).over->frame.has_value());
    // Shorthand `ROWS 2 PRECEDING` ends at the current row.
    EXPECT_EQ(call(4).over->frame->unit, WindowFrameUnit::ROWS);
    EXPECT_EQ(call(4).over->frame->start.kind, WindowBoundKind::PRECEDING);
    EXPECT_EQ(call(4).over->frame->end.kind, WindowBoundKind::CURRENT_ROW);
    const auto plain = std::get<SelectStatement>(parseQuery("SELECT UPPER(pv) FROM t"));
    EXPECT_EQ(std::get<FunctionCall>(plain.select_items[0].expression->value).over, nullptr);
}

TEST(WindowParserTest, WindowKeywordsRemainUsableAsColumnNames)
{
    // The time-series window input column and other soft keywords still parse as columns.
    const auto statement = std::get<SelectStatement>(parseQuery(
        "SELECT range, rows FROM mldp.time_series WHERE pv = 'A' AND window IN (TIMESTAMP_NS(0), TIMESTAMP_NS(10))"));
    ASSERT_EQ(statement.predicates.size(), 2U);
    EXPECT_EQ(std::get<InPredicate>(statement.predicates[1]).column.name, "window");
    EXPECT_EQ(statement.columns.size(), 2U);
    EXPECT_EQ(statement.columns[0].name, "range");
    EXPECT_NO_THROW((void)parseQuery("SELECT time AS window FROM t"));
    EXPECT_THROW((void)parseQuery("SELECT LAG(v) OVER FROM t"), ParseError);
    EXPECT_THROW((void)parseQuery("SELECT COUNT(*) OVER (ROWS BETWEEN 1 PRECEDING) FROM t"), ParseError);
}

TEST(WindowRegistryTest, ListsWindowFunctions)
{
    const auto functions = WindowFunctionRegistry::instance().functions();
    std::vector<std::string> names;
    for (const auto& function : functions)
    {
        names.push_back(function.name);
        EXPECT_EQ(function.kind, ExpressionCallableKind::WINDOW);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"dense_rank", "first_value", "lag", "last_value", "lead", "rank", "row_number"}));
    ASSERT_NE(WindowFunctionRegistry::instance().find("SUM"), nullptr);
    EXPECT_EQ(WindowFunctionRegistry::instance().find("SUM")->kind, WindowFunctionKind::AGGREGATE);
    EXPECT_EQ(WindowFunctionRegistry::instance().find("first"), nullptr);
}

// ---------------------------------------------------------------------------
// Planner and executor
// ---------------------------------------------------------------------------

TEST_F(WindowQueryTest, RankingAndOffsetFunctions)
{
    const auto rows = rowsOf(run(
        "SELECT pv, t, v, ROW_NUMBER() OVER w AS rn, RANK() OVER w AS rk, DENSE_RANK() OVER w AS dr, "
        "LAG(v) OVER w AS prev, LAG(v, 1, 0) OVER w AS prev0, LEAD(v) OVER w AS next, LEAD(v, 2) OVER w AS next2 "
        "FROM ws WINDOW w AS (PARTITION BY pv ORDER BY t)"));
    EXPECT_EQ(rows, sorted({
                        {"A", "1", "10", "1", "1", "1", "null", "0", "20", "null"},
                        {"A", "2", "20", "2", "2", "2", "10", "10", "null", "40"},
                        {"A", "3", "null", "3", "3", "3", "20", "20", "40", "50"},
                        {"A", "3", "40", "4", "3", "3", "null", "null", "50", "null"},
                        {"A", "5", "50", "5", "5", "4", "40", "40", "null", "null"},
                        {"B", "1", "5", "1", "1", "1", "null", "0", "7", "9"},
                        {"B", "2", "7", "2", "2", "2", "5", "5", "9", "null"},
                        // Null order keys sort last and are peers of each other.
                        {"B", "null", "9", "3", "3", "3", "7", "7", "null", "null"},
                    }));
}

TEST_F(WindowQueryTest, FrameAggregatesAndValueFunctions)
{
    const auto rows = rowsOf(run(
        "SELECT pv, t, v, "
        "SUM(v) OVER (PARTITION BY pv ORDER BY t) AS running, "
        "SUM(v) OVER (PARTITION BY pv ORDER BY t ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS pair, "
        "COUNT(*) OVER (PARTITION BY pv ORDER BY t RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS near, "
        "MIN(v) OVER (PARTITION BY pv ORDER BY t ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS lo, "
        "MAX(v) OVER (PARTITION BY pv ORDER BY t ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS hi, "
        "COUNT(v) OVER (PARTITION BY pv ORDER BY t) AS seen, "
        "FIRST_VALUE(v) OVER (PARTITION BY pv ORDER BY t) AS first, "
        "LAST_VALUE(v) OVER (PARTITION BY pv ORDER BY t) AS last "
        "FROM ws"));
    EXPECT_EQ(rows, sorted({
                        // Default RANGE frame includes the current row's peers (both t = 3 rows).
                        {"A", "1", "10", "10", "10", "2", "10", "20", "1", "10", "10"},
                        {"A", "2", "20", "30", "30", "4", "10", "20", "2", "10", "20"},
                        {"A", "3", "null", "70", "20", "3", "20", "40", "3", "10", "40"},
                        {"A", "3", "40", "70", "40", "3", "40", "50", "3", "10", "40"},
                        {"A", "5", "50", "120", "90", "1", "40", "50", "4", "10", "50"},
                        {"B", "1", "5", "5", "5", "2", "5", "7", "1", "5", "5"},
                        {"B", "2", "7", "12", "12", "2", "5", "9", "2", "5", "7"},
                        {"B", "null", "9", "21", "16", "1", "7", "9", "3", "5", "9"},
                    }));
}

TEST_F(WindowQueryTest, WholePartitionDescendingAndDurationFrames)
{
    const auto rows = rowsOf(run(
        "SELECT pv, t, v, AVG(v) OVER (PARTITION BY pv) AS mean, "
        "ROW_NUMBER() OVER (PARTITION BY pv ORDER BY t DESC) AS rn_desc, "
        "COUNT(*) OVER (PARTITION BY pv ORDER BY t DESC RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) AS up, "
        "COUNT(*) OVER (PARTITION BY pv ORDER BY ts RANGE BETWEEN 1s PRECEDING AND CURRENT ROW) AS recent "
        "FROM ws"));
    EXPECT_EQ(rows, sorted({
                        {"A", "1", "10", "30", "5", "2", "1"},
                        {"A", "2", "20", "30", "4", "3", "2"},
                        {"A", "3", "null", "30", "2", "2", "3"},
                        {"A", "3", "40", "30", "3", "2", "3"},
                        {"A", "5", "50", "30", "1", "1", "1"},
                        {"B", "1", "5", "7", "2", "2", "1"},
                        {"B", "2", "7", "7", "1", "1", "2"},
                        {"B", "null", "9", "7", "3", "1", "1"},
                    }));
}

TEST_F(WindowQueryTest, WindowsOverGroupedOutputAndDerivedTableFilter)
{
    const auto grouped = rowsOf(run(
        "SELECT pv, SUM(v) AS s, RANK() OVER (ORDER BY SUM(v) DESC) AS r, SUM(COUNT(*)) OVER () AS total FROM ws GROUP BY pv"));
    EXPECT_EQ(grouped, sorted({{"A", "120", "1", "8"}, {"B", "21", "2", "8"}}));

    // Filtering on a window result goes through a derived table.
    const auto latest = rowsOf(run(
        "SELECT pv, t FROM (SELECT pv, t, ROW_NUMBER() OVER (PARTITION BY pv ORDER BY t DESC) AS rn FROM ws) x WHERE rn = 1"));
    EXPECT_EQ(latest, sorted({{"A", "5"}, {"B", "2"}}));

    // ORDER BY may name a window call through its select alias.
    const auto last = rowsOf(run("SELECT pv, ROW_NUMBER() OVER (ORDER BY pv, t) AS rn FROM ws ORDER BY rn DESC LIMIT 1"), false);
    EXPECT_EQ(last, (Rows{{"B", "8"}}));

    // Window keys and arguments may come from joined tables.
    const auto joined = rowsOf(run("SELECT a.pv, COUNT(*) OVER (PARTITION BY a.pv) AS n, MAX(b.v) OVER (PARTITION BY a.pv) AS top "
                                   "FROM ws a JOIN ws b ON a.pv = b.pv"));
    ASSERT_EQ(joined.size(), 34U);
    EXPECT_EQ(joined.front(), (std::vector<std::string>{"A", "25", "50"}));
    EXPECT_EQ(joined.back(), (std::vector<std::string>{"B", "9", "9"}));

    // Streaming and materialized execution agree.
    const auto sql = "SELECT pv, t, LAG(v) OVER (PARTITION BY pv ORDER BY t) AS prev, SUM(v) OVER (PARTITION BY pv) AS total FROM ws";
    EXPECT_EQ(rowsOf(stream(sql)), rowsOf(run(sql)));
}

TEST_F(WindowQueryTest, RejectsInvalidWindowUsage)
{
    expectBindError("SELECT pv FROM ws WHERE v = ROW_NUMBER() OVER ()");
    expectBindError("SELECT COUNT(*) FROM ws GROUP BY ROW_NUMBER() OVER ()");
    expectBindError("SELECT pv FROM ws GROUP BY pv HAVING ROW_NUMBER() OVER () > 1");
    expectBindError("SELECT LAG(v) OVER nope FROM ws");
    expectBindError("SELECT LAG(v) OVER (w PARTITION BY t) FROM ws WINDOW w AS (PARTITION BY pv)");
    expectBindError("SELECT COUNT(*) OVER (ORDER BY pv, t RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM ws");
    expectBindError("SELECT COUNT(*) OVER (ORDER BY t RANGE BETWEEN 1s PRECEDING AND CURRENT ROW) FROM ws");
    expectBindError("SELECT COUNT(*) OVER (ORDER BY ts RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM ws");
    expectBindError("SELECT COUNT(*) OVER (ORDER BY t ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM ws");
    expectBindError("SELECT COUNT(DISTINCT v) OVER () FROM ws");
    expectBindError("SELECT FIRST(v) OVER () FROM ws");
    expectBindError("SELECT UPPER(pv) OVER () FROM ws");
    expectBindError("SELECT ROW_NUMBER(v) OVER () FROM ws");
    expectBindError("SELECT LAG(v, -1) OVER () FROM ws");
    expectBindError("SELECT LAG(ROW_NUMBER() OVER ()) OVER () FROM ws");
    expectBindError("SELECT ROW_NUMBER() FROM ws");
    expectBindError("SELECT pv FROM ws WINDOW w AS (), w AS ()");
}

// ---------------------------------------------------------------------------
// Operator: spill and cancellation
// ---------------------------------------------------------------------------

namespace {

plan::PhysicalWindow lagAndMovingAverage()
{
    plan::WindowGroup group{.partition_by = {column("pv")}, .order_by = {plan::SortKey{.column = "t", .expression = column("t"), .descending = false}}, .calls = {}};
    group.calls.push_back(plan::WindowCall{.function = "lag", .argument = column("v"), .offset = 1, .default_value = nullptr, .frame = {}, .name = "__win_0"});
    group.calls.push_back(plan::WindowCall{.function = "avg", .argument = column("v"), .offset = 1, .default_value = nullptr,
                                           .frame = plan::WindowFrameSpec{.rows = true, .start = WindowBoundKind::PRECEDING, .start_offset = 3,
                                                                          .end = WindowBoundKind::CURRENT_ROW, .end_offset = 0},
                                           .name = "__win_1"});
    return plan::PhysicalWindow{.input = nullptr, .groups = {std::move(group)}, .sorted_input = false};
}

RecordBatches generated(const int batches, const int rows_per_batch, const int partitions)
{
    RecordBatches input;
    for (int batch = 0; batch < batches; ++batch)
    {
        arrow::StringBuilder pv;
        arrow::Int64Builder  t;
        arrow::Int64Builder  v;
        for (int row = 0; row < rows_per_batch; ++row)
        {
            const auto index = batch * rows_per_batch + row;
            EXPECT_TRUE(pv.Append("PV:" + std::to_string(index % partitions)).ok());
            EXPECT_TRUE(t.Append(index).ok());
            EXPECT_TRUE(v.Append((index * 7) % 13).ok());
        }
        std::shared_ptr<arrow::Array> pv_array, t_array, v_array;
        EXPECT_TRUE(pv.Finish(&pv_array).ok());
        EXPECT_TRUE(t.Finish(&t_array).ok());
        EXPECT_TRUE(v.Finish(&v_array).ok());
        input.push_back(arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("t", arrow::int64()), arrow::field("v", arrow::int64())}),
                                                 rows_per_batch, {pv_array, t_array, v_array}));
    }
    return input;
}

} // namespace

TEST(WindowOperatorTest, SpillsSortedRunsWithIdenticalResults)
{
    const auto input = generated(20, 250, 40);
    const auto node = lagAndMovingAverage();
    const auto expected = rowsOf(applyWindow(input, node, ExecutionContext{.pool = arrow::default_memory_pool()}));
    ASSERT_EQ(expected.size(), 5000U);

    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    const ExecutionContext spilling{.pool = arrow::default_memory_pool(),
                                    .spill = std::make_shared<SpillManager>(file_system, "spill"),
                                    .memory_limit_bytes = 16 * 1024};
    const auto spilled = applyWindow(input, node, spilling);
    EXPECT_EQ(rowsOf(spilled), expected);
    // Output still comes partition by partition, ordered within each.
    const auto ordered = rowsOf(spilled, false);
    for (std::size_t row = 1; row < ordered.size(); ++row)
        if (ordered[row][0] == ordered[row - 1][0]) EXPECT_LT(std::stoll(ordered[row - 1][1]), std::stoll(ordered[row][1]));
}

TEST(WindowOperatorTest, PartitionLargerThanTheMemoryLimitFails)
{
    const auto input = generated(20, 250, 1);
    auto       file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    const ExecutionContext spilling{.pool = arrow::default_memory_pool(),
                                    .spill = std::make_shared<SpillManager>(file_system, "spill"),
                                    .memory_limit_bytes = 16 * 1024};
    EXPECT_THROW((void)applyWindow(input, lagAndMovingAverage(), spilling), std::runtime_error);
}

TEST(WindowOperatorTest, SortedInputIsVerifiedAndCancellationIsHonoured)
{
    // Rows arrive out of time order: the sorted-input hint must not change the result.
    auto       input = generated(4, 50, 5);
    std::reverse(input.begin(), input.end());
    auto       hinted = lagAndMovingAverage();
    hinted.sorted_input = true;
    const ExecutionContext context{.pool = arrow::default_memory_pool()};
    EXPECT_EQ(rowsOf(applyWindow(input, hinted, context)), rowsOf(applyWindow(input, lagAndMovingAverage(), context)));

    WindowRecordBatchStream stream(std::make_unique<MaterializedRecordBatchStream>(input), hinted, context);
    RecordBatches           streamed;
    while (auto batch = stream.next()) streamed.push_back(batch);
    EXPECT_EQ(rowsOf(streamed), rowsOf(applyWindow(input, hinted, context)));

    auto cancellation = std::make_shared<QueryCancellation>();
    cancellation->requestCancel();
    WindowRecordBatchStream cancelled(std::make_unique<MaterializedRecordBatchStream>(input), hinted,
                                      ExecutionContext{.pool = arrow::default_memory_pool(), .cancellation = cancellation});
    EXPECT_THROW((void)cancelled.next(), QueryCancelled);
}

// Throughput probe: 10M rows, 100 PVs, LAG + 5-minute moving average.
// Run explicitly with --gtest_also_run_disabled_tests.
TEST(WindowOperatorTest, DISABLED_Benchmark10MRows100Pvs)
{
    constexpr int kBatches = 160;
    constexpr int kRows = 62'500;
    RecordBatches input;
    for (int batch = 0; batch < kBatches; ++batch)
    {
        arrow::StringBuilder    pv;
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
        arrow::DoubleBuilder    value;
        for (int row = 0; row < kRows; ++row)
        {
            const int64_t index = static_cast<int64_t>(batch) * kRows + row;
            (void)pv.Append("PV:" + std::to_string(index % 100));
            (void)time.Append(index / 100 * 1'000'000'000LL);
            (void)value.Append(static_cast<double>(index % 997));
        }
        std::shared_ptr<arrow::Array> pv_array, time_array, value_array;
        (void)pv.Finish(&pv_array);
        (void)time.Finish(&time_array);
        (void)value.Finish(&value_array);
        input.push_back(arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("time", time_array->type()), arrow::field("value", arrow::float64())}),
                                                 kRows, {pv_array, time_array, value_array}));
    }
    plan::WindowGroup group{.partition_by = {column("pv")}, .order_by = {plan::SortKey{.column = "time", .expression = column("time"), .descending = false}}, .calls = {}};
    group.calls.push_back(plan::WindowCall{.function = "lag", .argument = column("value"), .offset = 1, .default_value = nullptr, .frame = {}, .name = "__win_0"});
    group.calls.push_back(plan::WindowCall{.function = "avg", .argument = column("value"), .offset = 1, .default_value = nullptr,
                                           .frame = plan::WindowFrameSpec{.rows = false, .start = WindowBoundKind::PRECEDING, .start_offset = 300'000'000'000LL,
                                                                          .end = WindowBoundKind::CURRENT_ROW, .end_offset = 0},
                                           .name = "__win_1"});
    const plan::PhysicalWindow node{.input = nullptr, .groups = {std::move(group)}, .sorted_input = true};
    const auto                 start = std::chrono::steady_clock::now();
    const auto                 output = applyWindow(input, node, ExecutionContext{.pool = arrow::default_memory_pool()});
    int64_t                    rows = 0;
    for (const auto& batch : output) rows += batch->num_rows();
    std::cout << "window benchmark: " << rows << " rows in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms\n";
    EXPECT_EQ(rows, static_cast<int64_t>(kBatches) * kRows);
}
