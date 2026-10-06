//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/AggregateRegistry.h>
#include <query/ExecutionContext.h>
#include <query/QueryCancellation.h>
#include <query/SpillManager.h>
#include <query/executor/AggregateRecordBatchStream.h>
#include <query/executor/GroupedAggregator.h>
#include <query/executor/MaterializedRecordBatchStream.h>
#include <query/plan/PlannerError.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/builder_union.h>
#include <arrow/filesystem/mockfs.h>
#include <arrow/memory_pool.h>
#include <arrow/scalar.h>
#include <arrow/table.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

namespace {

ExpressionPtr column(const std::string& name)
{
    return std::make_shared<Expression>(Expression{.value = QualifiedColumn{.qualifier = std::nullopt, .name = name, .path = {}}});
}

plan::AggregateCall call(const std::string& function, const std::string& argument, const std::string& name, const bool distinct = false)
{
    return plan::AggregateCall{.function = function, .argument = argument.empty() ? nullptr : column(argument), .distinct = distinct, .name = name};
}

std::shared_ptr<arrow::RecordBatch> samples(const std::vector<std::string>& pvs, const std::vector<std::optional<int64_t>>& values)
{
    arrow::StringBuilder pv_builder;
    arrow::Int64Builder  value_builder;
    for (std::size_t index = 0; index < pvs.size(); ++index)
    {
        EXPECT_TRUE(pv_builder.Append(pvs[index]).ok());
        EXPECT_TRUE((values[index] ? value_builder.Append(*values[index]) : value_builder.AppendNull()).ok());
    }
    std::shared_ptr<arrow::Array> pv;
    std::shared_ptr<arrow::Array> value;
    EXPECT_TRUE(pv_builder.Finish(&pv).ok());
    EXPECT_TRUE(value_builder.Finish(&value).ok());
    return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("value", arrow::int64())}),
                                    static_cast<int64_t>(pvs.size()), {pv, value});
}

/** Collects the aggregate output into {key text -> row of cell texts}. */
std::map<std::string, std::vector<std::string>> byKey(const RecordBatches& batches)
{
    std::map<std::string, std::vector<std::string>> rows;
    for (const auto& batch : batches)
    {
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            std::vector<std::string> cells;
            for (int column_index = 0; column_index < batch->num_columns(); ++column_index)
            {
                const auto& array = batch->column(column_index);
                cells.push_back(array->IsNull(row) ? "null" : (*array->GetScalar(row))->ToString());
            }
            rows[cells.front()] = cells;
        }
    }
    return rows;
}

plan::AggregateSpec perPvSpec()
{
    return plan::AggregateSpec{
        .keys = {plan::GroupKey{.expression = column("pv"), .name = "__key_0"}},
        .aggregates = {call("count", "", "__agg_0"), call("count", "value", "__agg_1"), call("sum", "value", "__agg_2"),
                       call("avg", "value", "__agg_3"), call("min", "value", "__agg_4"), call("max", "value", "__agg_5"),
                       call("first", "value", "__agg_6"), call("last", "value", "__agg_7"), call("count", "value", "__agg_8", true)},
        .having = nullptr};
}

void benchmarkAggregation(const int group_count, const bool count_only, const bool spill, const int batch_count = 16, const bool repeat_input = false)
{
    constexpr int rows_per_batch = 65536;
    const int64_t total_rows = static_cast<int64_t>(batch_count) * rows_per_batch;
    RecordBatches input;
    for (int batch_index = 0; batch_index < (repeat_input ? 1 : batch_count); ++batch_index)
    {
        std::vector<std::string> pvs;
        std::vector<std::optional<int64_t>> values;
        pvs.reserve(rows_per_batch);
        values.reserve(rows_per_batch);
        for (int row = 0; row < rows_per_batch; ++row)
        {
            const int64_t offset = static_cast<int64_t>(batch_index) * rows_per_batch + row;
            pvs.push_back("PV:" + std::to_string(offset % group_count));
            values.push_back(offset);
        }
        input.push_back(samples(pvs, values));
    }

    auto spec = perPvSpec();
    if (count_only) spec.aggregates.resize(1);
    arrow::ProxyMemoryPool pool(arrow::default_memory_pool());
    ExecutionContext context{.pool = &pool};
    std::shared_ptr<arrow::fs::internal::MockFileSystem> file_system;
    if (spill)
    {
        file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
        context.spill = std::make_shared<SpillManager>(file_system, "benchmark-spill");
        context.memory_limit_bytes = 1024 * 1024;
        context.spill_partitions = 16;
    }

    const auto start = std::chrono::steady_clock::now();
    GroupedAggregator aggregator(spec, context);
    for (int batch_index = 0; batch_index < batch_count; ++batch_index)
        aggregator.consume(input[repeat_input ? 0 : batch_index]);
    const auto output = aggregator.finish();
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    int64_t output_groups = 0;
    int64_t counted_rows = 0;
    for (const auto& batch : output)
    {
        ASSERT_TRUE(batch->ValidateFull().ok());
        output_groups += batch->num_rows();
        const auto counts = std::static_pointer_cast<arrow::Int64Array>(batch->column(1));
        for (int64_t row = 0; row < batch->num_rows(); ++row) counted_rows += counts->Value(row);
    }
    EXPECT_EQ(output_groups, group_count);
    EXPECT_EQ(counted_rows, total_rows);
    std::cout << "rows=" << total_rows << " groups=" << output_groups << " aggregates=" << spec.aggregates.size()
              << " input=" << (repeat_input ? "repeated-batch" : "varying-batches")
              << " spill=" << (spill ? "mock-fs" : "none") << " seconds=" << seconds
              << " Mrows/s=" << total_rows / seconds / 1e6 << " aggregate_arrow_peak_MB=" << pool.max_memory() / 1e6 << "\n";
    if (context.spill) EXPECT_TRUE(context.spill->cleanup().ok());
}

} // namespace

TEST(AggregateRegistryTest, ListsBuiltInsAndBindsTypes)
{
    const auto& registry = AggregateRegistry::instance();
    std::vector<std::string> names;
    for (const auto& descriptor : registry.functions())
    {
        names.push_back(descriptor.name);
        EXPECT_EQ(descriptor.kind, ExpressionCallableKind::AGGREGATE);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"avg", "count", "first", "last", "max", "min", "sum"}));
    ASSERT_NE(registry.find("COUNT"), nullptr);
    EXPECT_EQ(registry.find("to_utc"), nullptr);
    EXPECT_EQ(registry.find("count")->bind(std::nullopt, false), ColumnType::INT);
    EXPECT_EQ(registry.find("max")->bind(ColumnType::TIMESTAMP, false), ColumnType::TIMESTAMP);
    EXPECT_THROW((void)registry.find("sum")->bind(ColumnType::STRING, false), plan::PlannerException);
    EXPECT_THROW((void)registry.find("max")->bind(std::nullopt, false), plan::PlannerException);
    EXPECT_THROW((void)registry.find("min")->bind(ColumnType::INT, true), plan::PlannerException);
}

TEST(GroupedAggregatorTest, ComputesEveryAggregateAcrossBatches)
{
    const ExecutionContext context{.pool = arrow::default_memory_pool()};
    const auto output = applyAggregate({samples({"A", "B", "A"}, {1, 2, 4}), samples({"A", "C", "B"}, {4, std::nullopt, 7})}, perPvSpec(), context);
    const auto rows = byKey(output);
    ASSERT_EQ(rows.size(), 3U);
    // pv, count(*), count, sum, avg, min, max, first, last, count distinct
    EXPECT_EQ(rows.at("A"), (std::vector<std::string>{"A", "3", "3", "9", "3", "1", "4", "1", "4", "2"}));
    EXPECT_EQ(rows.at("B"), (std::vector<std::string>{"B", "2", "2", "9", "4.5", "2", "7", "2", "7", "2"}));
    EXPECT_EQ(rows.at("C"), (std::vector<std::string>{"C", "1", "0", "null", "null", "null", "null", "null", "null", "0"}));
    // Groups keep first-appearance order.
    ASSERT_EQ(output.size(), 1U);
    EXPECT_EQ((*output.front()->column(0)->GetScalar(1))->ToString(), "B");
    // MIN/MAX keep the input type.
    EXPECT_EQ(output.front()->column(5)->type()->id(), arrow::Type::INT64);
}

TEST(GroupedAggregatorTest, GlobalGroupOnEmptyInputAndHaving)
{
    const ExecutionContext context{.pool = arrow::default_memory_pool()};
    const plan::AggregateSpec global{.keys = {}, .aggregates = {call("count", "", "__agg_0"), call("sum", "value", "__agg_1")}, .having = nullptr};
    const auto empty = applyAggregate({}, global, context);
    ASSERT_EQ(empty.size(), 1U);
    EXPECT_EQ((*empty.front()->column(0)->GetScalar(0))->ToString(), "0");
    EXPECT_TRUE(empty.front()->column(1)->IsNull(0));

    auto spec = perPvSpec();
    spec.having = std::make_shared<Expression>(Expression{.value = BinaryExpression{">", column("__agg_0"), std::make_shared<Expression>(Expression{.value = LiteralValue{int64_t{1}}})}});
    const auto rows = byKey(applyAggregate({samples({"A", "B", "A", "C"}, {1, 2, 3, 4})}, spec, context));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_TRUE(rows.contains("A"));
}

TEST(GroupedAggregatorTest, AggregatesNativeUnionValuesAndKeepsTimestampType)
{
    const auto value_type = arrow::dense_union({arrow::field("double", arrow::float64()), arrow::field("int64", arrow::int64())});
    auto doubles = std::make_shared<arrow::DoubleBuilder>();
    auto ints = std::make_shared<arrow::Int64Builder>();
    arrow::DenseUnionBuilder union_builder(arrow::default_memory_pool(), {doubles, ints}, value_type);
    ASSERT_TRUE(union_builder.Append(0).ok());
    ASSERT_TRUE(doubles->Append(1.5).ok());
    ASSERT_TRUE(union_builder.Append(1).ok());
    ASSERT_TRUE(ints->Append(4).ok());
    ASSERT_TRUE(union_builder.Append(0).ok());
    ASSERT_TRUE(doubles->Append(0.5).ok());
    std::shared_ptr<arrow::Array> values;
    ASSERT_TRUE(union_builder.Finish(&values).ok());

    arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
    ASSERT_TRUE(time_builder.AppendValues({30, 10, 20}).ok());
    std::shared_ptr<arrow::Array> times;
    ASSERT_TRUE(time_builder.Finish(&times).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", value_type), arrow::field("time", times->type())}), 3, {values, times});

    const plan::AggregateSpec spec{.keys = {},
                                   .aggregates = {call("sum", "value", "__agg_0"), call("avg", "value", "__agg_1"), call("max", "value", "__agg_2"),
                                                  call("min", "time", "__agg_3")},
                                   .having = nullptr};
    const auto output = applyAggregate({batch}, spec, ExecutionContext{.pool = arrow::default_memory_pool()});
    ASSERT_EQ(output.size(), 1U);
    const auto& row = output.front();
    EXPECT_EQ((*row->column(0)->GetScalar(0))->ToString(), "6");
    EXPECT_EQ((*row->column(1)->GetScalar(0))->ToString(), "2");
    EXPECT_EQ((*row->column(2)->GetScalar(0))->ToString(), "4");
    EXPECT_TRUE(row->column(3)->type()->Equals(*times->type()));
    EXPECT_EQ(std::static_pointer_cast<arrow::TimestampScalar>(*row->column(3)->GetScalar(0))->value, 10);
}

TEST(GroupedAggregatorTest, SpillsPastTheMemoryBudgetWithIdenticalResults)
{
    // Many distinct keys spread over many batches, each key seen several times.
    RecordBatches input;
    for (int batch = 0; batch < 20; ++batch)
    {
        std::vector<std::string>            pvs;
        std::vector<std::optional<int64_t>> values;
        for (int row = 0; row < 500; ++row)
        {
            pvs.push_back("PV:" + std::to_string((batch * 500 + row) % 3000));
            values.push_back(batch * 500 + row);
        }
        input.push_back(samples(pvs, values));
    }

    const auto expected = byKey(applyAggregate(input, perPvSpec(), ExecutionContext{.pool = arrow::default_memory_pool()}));
    ASSERT_EQ(expected.size(), 3000U);

    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    const ExecutionContext spilling{.pool = arrow::default_memory_pool(),
                                    .spill = std::make_shared<SpillManager>(file_system, "spill"),
                                    .memory_limit_bytes = 16 * 1024,
                                    .spill_partitions = 4};
    const auto actual = byKey(applyAggregate(input, perPvSpec(), spilling));
    // FIRST/LAST depend on row order, which spilling preserves within a key.
    EXPECT_EQ(actual, expected);
}

TEST(GroupedAggregatorTest, StreamingOperatorMatchesMaterializedPathAndHonoursCancellation)
{
    const RecordBatches input{samples({"A", "B"}, {1, 2}), samples({"B", "A"}, {3, 4})};
    const plan::PhysicalAggregate node{.input = nullptr, .spec = perPvSpec()};
    const ExecutionContext        context{.pool = arrow::default_memory_pool()};

    AggregateRecordBatchStream stream(std::make_unique<MaterializedRecordBatchStream>(input), node, context);
    RecordBatches              streamed;
    while (auto batch = stream.next()) streamed.push_back(batch);
    EXPECT_EQ(byKey(streamed), byKey(applyAggregate(input, perPvSpec(), context)));

    auto cancellation = std::make_shared<QueryCancellation>();
    cancellation->requestCancel();
    AggregateRecordBatchStream cancelled(std::make_unique<MaterializedRecordBatchStream>(input), node,
                                         ExecutionContext{.pool = arrow::default_memory_pool(), .cancellation = cancellation});
    EXPECT_THROW((void)cancelled.next(), QueryCancelled);
}

// Throughput / memory probe; run explicitly with --gtest_also_run_disabled_tests.
TEST(GroupedAggregatorTest, DISABLED_Benchmark10MRows100Groups)
{
    const auto spec_name = std::getenv("AGG_BENCH_SPEC");
    benchmarkAggregation(100, spec_name != nullptr && std::string(spec_name) == "count", false, 160, true);
}

TEST(GroupedAggregatorTest, DISABLED_Benchmark1MRows10KGroups)
{
    benchmarkAggregation(10000, false, false);
}

TEST(GroupedAggregatorTest, DISABLED_Benchmark1MRows100KGroups)
{
    benchmarkAggregation(100000, false, false);
}

TEST(GroupedAggregatorTest, DISABLED_Benchmark1MRows100KGroupsCountOnly)
{
    benchmarkAggregation(100000, true, false);
}

TEST(GroupedAggregatorTest, DISABLED_Benchmark1MRows100KGroupsMockSpill)
{
    benchmarkAggregation(100000, false, true);
}
