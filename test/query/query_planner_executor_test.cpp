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
#include <query/QueryExecutor.h>
#include <query/executor/ExecutionState.h>
#include <query/QueryPlanner.h>
#include <query/QueryResult.h>
#include <query/QueryTableCatalog.h>
#include <query/QueryableFactory.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PlannerError.h>

#include <config/Config.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/builder_union.h>
#include <arrow/memory_pool.h>
#include <arrow/filesystem/mockfs.h>
#include <arrow/scalar.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string_view>
#include <variant>

namespace plan = mldp_pvxs_driver::query::plan;

using namespace mldp_pvxs_driver;

namespace {

class FakeQueryable : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;

    explicit FakeQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view table_name) const override
    {
        if (table_name == "fake.paged")
        {
            return {
                {"pv", query::ColumnType::STRING, false, true, {}, {}, "Paged test value"},
            };
        }
        if (table_name == "fake.meta")
        {
            return {
                {"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"owner", query::ColumnType::STRING, false, true, {}, {query::PredicateOp::LIKE}, "owner"},
                {"tags", query::ColumnType::STRING, false, true, {}, {}, "tags"},
                {"attributes", query::ColumnType::STRING, false, true, {}, {}, "attributes"},
                {"tag", query::ColumnType::STRING, false, false, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "tag membership"},
            };
        }

        return {
            {"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
            {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
            {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::EQ, query::PredicateOp::IN}, "value"},
        };
    }

    query::QueryResult execute(std::string_view                     table_name,
                               const std::vector<query::Predicate>& pushable_predicates,
                               const std::set<std::string>&         projection_hint,
                               const query::ExecutionContext&,
                               std::string_view page_token = {}) override
    {
        if (table_name == "fake.paged")
        {
            arrow::StringBuilder pv_builder;
            EXPECT_TRUE(pv_builder.Append(page_token.empty() ? "A" : "B").ok());
            std::shared_ptr<arrow::Array> pv;
            EXPECT_TRUE(pv_builder.Finish(&pv).ok());
            return query::QueryResult{
                .batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8())}), 1, {pv}),
                .next_page_token = page_token.empty() ? "second-page" : ""};
        }
        if (table_name == "fake.meta")
        {
            arrow::StringBuilder pv_builder;
            arrow::StringBuilder owner_builder;
            arrow::StringBuilder tags_builder;
            arrow::StringBuilder attributes_builder;
            arrow::StringBuilder device_group_builder;

            const auto include_row = [&](const std::string& pv) -> bool
            {
                for (const auto& predicate : pushable_predicates)
                {
                    if (predicate.column == "pv" &&
                        predicate.op == query::PredicateOp::EQ &&
                        !predicate.values.empty() &&
                        std::holds_alternative<std::string>(predicate.values.front()) &&
                        std::get<std::string>(predicate.values.front()) != pv)
                    {
                        return false;
                    }
                    if (predicate.column == "pv" && predicate.op == query::PredicateOp::IN)
                    {
                        bool matched = false;
                        for (const auto& value : predicate.values)
                        {
                            if (std::holds_alternative<std::string>(value) && std::get<std::string>(value) == pv)
                            {
                                matched = true;
                                break;
                            }
                        }
                        if (!matched)
                        {
                            return false;
                        }
                    }
                }
                return true;
            };

            if (include_row("A"))
            {
                EXPECT_TRUE(pv_builder.Append("A").ok());
                EXPECT_TRUE(owner_builder.Append("alice").ok());
                EXPECT_TRUE(tags_builder.Append("sample").ok());
                EXPECT_TRUE(attributes_builder.Append("device_group=RF").ok());
                EXPECT_TRUE(device_group_builder.Append("RF").ok());
            }
            if (include_row("C"))
            {
                EXPECT_TRUE(pv_builder.Append("C").ok());
                EXPECT_TRUE(owner_builder.Append("carol").ok());
                EXPECT_TRUE(tags_builder.Append("configuration").ok());
                EXPECT_TRUE(attributes_builder.Append("device_group=MAGNET").ok());
                EXPECT_TRUE(device_group_builder.Append("MAGNET").ok());
            }

            std::shared_ptr<arrow::Array> pv;
            std::shared_ptr<arrow::Array> owner;
            std::shared_ptr<arrow::Array> tags;
            std::shared_ptr<arrow::Array> attributes;
            std::shared_ptr<arrow::Array> device_group;
            EXPECT_TRUE(pv_builder.Finish(&pv).ok());
            EXPECT_TRUE(owner_builder.Finish(&owner).ok());
            EXPECT_TRUE(tags_builder.Finish(&tags).ok());
            EXPECT_TRUE(attributes_builder.Finish(&attributes).ok());
            EXPECT_TRUE(device_group_builder.Finish(&device_group).ok());

            std::vector<std::shared_ptr<arrow::Field>> fields;
            std::vector<std::shared_ptr<arrow::Array>> columns;
            if (projection_hint.empty() || projection_hint.contains("pv"))
            {
                fields.push_back(arrow::field("pv", arrow::utf8()));
                columns.push_back(pv);
            }
            if (projection_hint.empty() || projection_hint.contains("owner"))
            {
                fields.push_back(arrow::field("owner", arrow::utf8()));
                columns.push_back(owner);
            }
            if (projection_hint.empty() || projection_hint.contains("tags"))
            {
                fields.push_back(arrow::field("tags", arrow::utf8()));
                columns.push_back(tags);
            }
            if (projection_hint.empty() || projection_hint.contains("attributes"))
            {
                fields.push_back(arrow::field("attributes", arrow::utf8()));
                columns.push_back(attributes);
            }
            if (projection_hint.empty() || projection_hint.contains("attributes.device_group"))
            {
                fields.push_back(arrow::field("attributes.device_group", arrow::utf8()));
                columns.push_back(device_group);
            }

            return query::QueryResult{
                .batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), pv->length(), std::move(columns)),
                .next_page_token = ""};
        }

        arrow::StringBuilder pv_builder;
        arrow::Int64Builder  time_builder;
        arrow::Int64Builder  value_builder;

        const auto include_row = [&](const std::string& pv, const int64_t time) -> bool
        {
            for (const auto& predicate : pushable_predicates)
            {
                if (predicate.column == "pv" &&
                    predicate.op == query::PredicateOp::EQ &&
                    !predicate.values.empty() &&
                    std::holds_alternative<std::string>(predicate.values.front()) &&
                    std::get<std::string>(predicate.values.front()) != pv)
                {
                    return false;
                }
                if (predicate.column == "time" &&
                    predicate.op == query::PredicateOp::GTE &&
                    !predicate.values.empty() &&
                    std::holds_alternative<int64_t>(predicate.values.front()) &&
                    time < std::get<int64_t>(predicate.values.front()))
                {
                    return false;
                }
                if (predicate.column == "pv" && predicate.op == query::PredicateOp::IN)
                {
                    bool matched = false;
                    for (const auto& value : predicate.values)
                    {
                        if (std::holds_alternative<std::string>(value) && std::get<std::string>(value) == pv)
                        {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) return false;
                }
            }
            return true;
        };

        if (include_row("A", 10))
        {
            EXPECT_TRUE(pv_builder.Append("A").ok());
            EXPECT_TRUE(time_builder.Append(10).ok());
            EXPECT_TRUE(value_builder.Append(1).ok());
        }
        if (include_row("B", 20))
        {
            EXPECT_TRUE(pv_builder.Append("B").ok());
            EXPECT_TRUE(time_builder.Append(20).ok());
            EXPECT_TRUE(value_builder.Append(2).ok());
        }

        std::shared_ptr<arrow::Array> pv;
        std::shared_ptr<arrow::Array> time;
        std::shared_ptr<arrow::Array> value;
        EXPECT_TRUE(pv_builder.Finish(&pv).ok());
        EXPECT_TRUE(time_builder.Finish(&time).ok());
        EXPECT_TRUE(value_builder.Finish(&value).ok());

        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> columns;
        if (projection_hint.empty() || projection_hint.contains("pv"))
        {
            fields.push_back(arrow::field("pv", arrow::utf8()));
            columns.push_back(pv);
        }
        if (projection_hint.empty() || projection_hint.contains("time"))
        {
            fields.push_back(arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND, "UTC")));
            columns.push_back(time);
        }
        if (projection_hint.empty() || projection_hint.contains("value"))
        {
            fields.push_back(arrow::field("value", arrow::int64()));
            columns.push_back(value);
        }

        return query::QueryResult{
            .batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), pv->length(), std::move(columns)),
            .next_page_token = ""};
    }
};

class EmptyWideInputQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;
    inline static uint64_t                  execute_calls{0};

    explicit EmptyWideInputQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(const std::string_view table_name) const override
    {
        if (table_name == "mldp.pv_metadata")
        {
            return {{"pv", query::ColumnType::STRING, false, true, {query::PredicateOp::EQ}, {}, "PV"}};
        }
        if (table_name == "mldp.configuration_activation")
        {
            return {
                {"time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "time"},
                {"end_time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "end time"},
                {"activation_id", query::ColumnType::STRING, false, true, {query::PredicateOp::EQ}, {}, "activation id"},
            };
        }
        return {
            {"pv", query::ColumnType::STRING, true, false, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV input"},
            {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
            {"window", query::ColumnType::TIMESTAMP, false, false, {query::PredicateOp::IN}, {}, "window input"},
        };
    }

    query::QueryResult execute(std::string_view,
                               const std::vector<query::Predicate>& predicates,
                               const std::set<std::string>&,
                               const query::ExecutionContext&,
                               std::string_view = {}) override
    {
        ++execute_calls;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "activation_id" || predicate.column == "pv")
            {
                return {};
            }
        }
        return {};
    }
};

const std::set<std::string_view> FakeQueryable::kVirtualTables = {"fake.samples", "fake.meta", "fake.paged"};
const std::set<std::string_view> EmptyWideInputQueryable::kVirtualTables = {
    "mldp.time_series", "mldp.time_series_table", "mldp.pv_metadata", "mldp.configuration_activation"};

class PlannerExecutorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        query::QueryableFactory::instance().reset();
        query::QueryableFactory::instance().prepare<FakeQueryable>(config::Config::configFromYamlString("{}"));
    }

    void TearDown() override
    {
        query::QueryableFactory::instance().reset();
    }
};

TEST(EmptyWideInputTest, EmptyValidSubqueriesProduceNoWideTableRequest)
{
    query::QueryableFactory::instance().reset();
    EmptyWideInputQueryable::execute_calls = 0;
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto pv_empty = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT * FROM mldp.time_series_table WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv = 'NO:PV')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(pv_empty.batches.empty());
    EXPECT_EQ(pv_empty.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);

    EmptyWideInputQueryable::execute_calls = 0;
    const auto window_empty = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT * FROM mldp.time_series_table WHERE pv = 'PV:ONE' "
            "AND window IN (SELECT time, end_time FROM mldp.configuration_activation WHERE activation_id = 'none')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(window_empty.batches.empty());
    EXPECT_EQ(window_empty.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);
    query::QueryableFactory::instance().reset();
}

TEST(EmptyWideInputTest, EmptyPvSubqueryProducesNoNarrowTimeSeriesRequest)
{
    query::QueryableFactory::instance().reset();
    EmptyWideInputQueryable::execute_calls = 0;
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto result = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT pv, time FROM mldp.time_series "
            "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv = 'NO:PV')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(result.batches.empty());
    EXPECT_EQ(result.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);
    query::QueryableFactory::instance().reset();
}

const plan::PhysicalTableScan* findScan(const plan::PhysicalNodePtr& node)
{
    if (!node)
    {
        return nullptr;
    }
    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        return scan;
    }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        return findScan(filter->input);
    }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        return findScan(project->input);
    }
    if (const auto* sort = std::get_if<plan::PhysicalSort>(&node->value))
    {
        return findScan(sort->input);
    }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        return findScan(limit->input);
    }
    return nullptr;
}

TEST_F(PlannerExecutorTest, PlansSelectWithBackboneNodes)
{
    query::QueryPlanner planner;
    const auto          statement = query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' LIMIT 1");
    const auto          plan = planner.plan(statement);
    const auto          text = query::plan::physicalPlanToString(plan);
    EXPECT_NE(text.find("PhysicalLimit"), std::string::npos);
    EXPECT_NE(text.find("PhysicalProject"), std::string::npos);
    EXPECT_NE(text.find("PhysicalTableScan"), std::string::npos);
}

TEST_F(PlannerExecutorTest, InitializesMatchingExecutionStateTree)
{
    const auto scan = plan::makeNode(plan::PhysicalTableScan{.table_name = "fake.samples"});
    const auto filter = plan::makeNode(plan::PhysicalFilter{
        .input = scan,
        .predicates = {query::Predicate{.column = "pv", .op = query::PredicateOp::EQ, .values = {std::string("A")}}}});
    const auto project = plan::makeNode(plan::PhysicalProject{.input = filter, .columns = {"pv"}});
    query::QueryStats stats;
    const auto state = query::executor::makeExecutionState(
        project, {.pool = arrow::default_memory_pool()}, stats);

    ASSERT_EQ(state->typeName(), "ProjectExecutionState");
    ASSERT_EQ(state->children().size(), 1U);
    EXPECT_EQ(state->children().front()->typeName(), "FilterExecutionState");
    ASSERT_EQ(state->children().front()->children().size(), 1U);
    EXPECT_EQ(state->children().front()->children().front()->typeName(), "TableScanExecutionState");
}

TEST_F(PlannerExecutorTest, ExecutionStateFactoryMapsAllPhysicalNodeTypes)
{
    const auto scan = plan::makeNode(plan::PhysicalTableScan{.table_name = "fake.samples"});
    const std::vector<std::pair<plan::PhysicalNodePtr, std::string_view>> cases{
        {scan, "TableScanExecutionState"},
        {plan::makeNode(plan::PhysicalFilter{.input = scan}), "FilterExecutionState"},
        {plan::makeNode(plan::PhysicalProject{.input = scan}), "ProjectExecutionState"},
        {plan::makeNode(plan::PhysicalSort{.input = scan}), "SortExecutionState"},
        {plan::makeNode(plan::PhysicalLimit{.input = scan}), "LimitExecutionState"},
        {plan::makeNode(plan::PhysicalHashJoin{.left = scan, .right = scan}), "HashJoinExecutionState"},
        {plan::makeNode(plan::PhysicalNestedLoopJoin{.outer = scan, .inner = scan}), "NestedLoopJoinExecutionState"},
        {plan::makeNode(plan::PhysicalBlockNestedLoopJoin{.outer = scan, .inner = scan}), "BlockNestedLoopJoinExecutionState"},
        {plan::makeNode(plan::PhysicalShowTables{}), "ShowTablesExecutionState"},
        {plan::makeNode(plan::PhysicalDescribe{}), "DescribeExecutionState"},
        {plan::makeNode(plan::PhysicalExplain{}), "ExplainExecutionState"},
        {plan::makeNode(plan::PhysicalCreateTable{.query = scan}), "CreateTableExecutionState"},
        {plan::makeNode(plan::PhysicalDropTable{}), "DropTableExecutionState"},
    };
    for (const auto& [physical, expected] : cases)
    {
        query::QueryStats stats;
        const auto state = query::executor::makeExecutionState(
            physical, {.pool = arrow::default_memory_pool()}, stats);
        EXPECT_EQ(state->typeName(), expected);
    }
}

TEST_F(PlannerExecutorTest, ExecutesDerivedSourceAndPlansItAsArrowIpcScan)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner planner(catalog);
    query::QueryExecutor executor;

    const auto create = planner.plan(query::parseQuery("CREATE TEMP TABLE samples AS SELECT pv, value FROM fake.samples WHERE pv = 'A'"));
    const auto created = executor.execute(create, context);
    EXPECT_EQ(created.stats.rpc_calls, 1U);

    const auto stored = executor.execute(planner.plan(query::parseQuery("SELECT pv FROM samples WHERE value = 1")), context);
    EXPECT_EQ(stored.stats.rpc_calls, 0U);
    ASSERT_EQ(stored.batches.size(), 1U);
    EXPECT_EQ(stored.batches.front()->num_rows(), 1);

    const auto derived = executor.execute(planner.plan(query::parseQuery("SELECT recent.pv FROM (SELECT pv FROM samples) recent")), context);
    EXPECT_EQ(derived.stats.rpc_calls, 0U);
    ASSERT_EQ(derived.batches.size(), 1U);
    EXPECT_EQ(derived.batches.front()->num_rows(), 1);

    const auto empty_create = planner.plan(query::parseQuery("CREATE TEMP TABLE empty_samples AS SELECT pv, value FROM fake.samples WHERE pv = 'missing'"));
    const auto empty_created = executor.execute(empty_create, context);
    EXPECT_EQ(empty_created.stats.rpc_calls, 1U);

    const auto empty_stored = executor.execute(planner.plan(query::parseQuery("SELECT * FROM empty_samples")), context);
    EXPECT_EQ(empty_stored.stats.rpc_calls, 0U);
    ASSERT_EQ(empty_stored.batches.size(), 1U);
    ASSERT_NE(empty_stored.batches.front(), nullptr);
    EXPECT_EQ(empty_stored.batches.front()->num_rows(), 0);
    EXPECT_EQ(empty_stored.batches.front()->schema()->field(0)->name(), "pv");
    EXPECT_EQ(empty_stored.batches.front()->schema()->field(1)->name(), "value");
}

TEST_F(PlannerExecutorTest, FiltersMaterializedDenseUnionValuesNumerically)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    const auto value_type = arrow::dense_union({arrow::field("string", arrow::utf8()), arrow::field("double", arrow::float64())});
    auto string_builder = std::make_shared<arrow::StringBuilder>();
    auto double_builder = std::make_shared<arrow::DoubleBuilder>();
    arrow::DenseUnionBuilder value_builder(arrow::default_memory_pool(), {string_builder, double_builder}, value_type);
    ASSERT_TRUE(value_builder.Append(0).ok());
    ASSERT_TRUE(string_builder->Append("not numeric").ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(double_builder->Append(9.5).ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(double_builder->Append(10.5).ok());
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(value_builder.Finish(&value).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", value_type)}), 3, {value});
    ASSERT_TRUE(catalog->create("magnet_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner planner(catalog);
    query::QueryExecutor executor;
    const auto result = executor.execute(planner.plan(query::parseQuery("SELECT * FROM magnet_samples WHERE value > 10.0")), context);

    EXPECT_EQ(result.stats.rpc_calls, 0U);
    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->schema()->field(0)->type()->id(), arrow::Type::DENSE_UNION);
    const auto scalar = result.batches.front()->column(0)->GetScalar(0);
    ASSERT_TRUE(scalar.ok());
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*scalar)->child_value()->ToString(), "10.5");
}

TEST_F(PlannerExecutorTest, FiltersMaterializedNativeUnionValuesByActiveTypeWithoutUnionGather)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    const auto value_type = arrow::dense_union({arrow::field("string", arrow::utf8()), arrow::field("bool", arrow::boolean()),
                                                arrow::field("int", arrow::int32()), arrow::field("float", arrow::float32()),
                                                arrow::field("double", arrow::float64()), arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO)),
                                                arrow::field("binary", arrow::binary())});
    auto string_builder = std::make_shared<arrow::StringBuilder>();
    auto bool_builder = std::make_shared<arrow::BooleanBuilder>();
    auto int_builder = std::make_shared<arrow::Int32Builder>();
    auto float_builder = std::make_shared<arrow::FloatBuilder>();
    auto double_builder = std::make_shared<arrow::DoubleBuilder>();
    auto timestamp_builder = std::make_shared<arrow::TimestampBuilder>(
        arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
    auto binary_builder = std::make_shared<arrow::BinaryBuilder>();
    arrow::DenseUnionBuilder value_builder(arrow::default_memory_pool(), {string_builder, bool_builder, int_builder, float_builder, double_builder, timestamp_builder, binary_builder}, value_type);
    ASSERT_TRUE(value_builder.Append(0).ok()); ASSERT_TRUE(string_builder->Append("11").ok());
    ASSERT_TRUE(value_builder.Append(1).ok()); ASSERT_TRUE(bool_builder->Append(true).ok());
    ASSERT_TRUE(value_builder.Append(2).ok()); ASSERT_TRUE(int_builder->Append(10).ok());
    ASSERT_TRUE(value_builder.Append(3).ok()); ASSERT_TRUE(float_builder->Append(10.5F).ok());
    ASSERT_TRUE(value_builder.Append(4).ok()); ASSERT_TRUE(double_builder->Append(11.0).ok());
    ASSERT_TRUE(value_builder.Append(5).ok()); ASSERT_TRUE(timestamp_builder->Append(11).ok());
    ASSERT_TRUE(value_builder.Append(6).ok()); ASSERT_TRUE(binary_builder->Append("11").ok());
    ASSERT_TRUE(value_builder.AppendNull().ok());
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(value_builder.Finish(&value).ok());

    arrow::StringBuilder pv_builder;
    arrow::Int64Builder time_builder;
    for (int row = 0; row < 8; ++row)
    {
        ASSERT_TRUE(pv_builder.Append("PV:" + std::to_string(row)).ok());
        ASSERT_TRUE(time_builder.Append(100 + row).ok());
    }
    std::shared_ptr<arrow::Array> pv;
    std::shared_ptr<arrow::Array> time;
    ASSERT_TRUE(pv_builder.Finish(&pv).ok());
    ASSERT_TRUE(time_builder.Finish(&time).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("time", arrow::int64()), arrow::field("value", value_type)}), 8, {pv, time, value});
    ASSERT_TRUE(catalog->create("magnet_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner planner(catalog);
    query::QueryExecutor executor;
    const auto execute = [&](const std::string_view sql)
    {
        const auto result = executor.execute(planner.plan(query::parseQuery(sql)), context);
        EXPECT_EQ(result.stats.rpc_calls, 0U);
        EXPECT_EQ(result.batches.size(), 1U);
        return result.batches.empty() ? std::shared_ptr<arrow::RecordBatch>{} : result.batches.front();
    };

    const auto greater = execute("SELECT * FROM magnet_samples WHERE value > 10.0");
    ASSERT_NE(greater, nullptr);
    ASSERT_EQ(greater->num_rows(), 2);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringScalar>(*greater->column(0)->GetScalar(0))->ToString(), "PV:3");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringScalar>(*greater->column(0)->GetScalar(1))->ToString(), "PV:4");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::Int64Scalar>(*greater->column(1)->GetScalar(0))->value, 103);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::Int64Scalar>(*greater->column(1)->GetScalar(1))->value, 104);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*greater->column(2)->GetScalar(0))->child_value()->ToString(), "10.5");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*greater->column(2)->GetScalar(1))->child_value()->ToString(), "11");

    const auto not_equal = execute("SELECT * FROM magnet_samples WHERE value != 10.0");
    const auto in = execute("SELECT * FROM magnet_samples WHERE value IN (10.0, 11.0)");
    const auto between = execute("SELECT * FROM magnet_samples WHERE value BETWEEN 10.0 AND 10.5");
    const auto equal = execute("SELECT * FROM magnet_samples WHERE value = 11.0");
    ASSERT_NE(not_equal, nullptr);
    ASSERT_NE(in, nullptr);
    ASSERT_NE(between, nullptr);
    ASSERT_NE(equal, nullptr);
    EXPECT_EQ(not_equal->num_rows(), 2);
    EXPECT_EQ(in->num_rows(), 2);
    EXPECT_EQ(between->num_rows(), 2);
    EXPECT_EQ(equal->num_rows(), 1);
}

TEST_F(PlannerExecutorTest, FiltersNativeTimestampAndDurationUnionValuesWithTypedLiterals)
{
    auto file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    const auto value_type = arrow::dense_union({arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO)),
                                                arrow::field("duration", arrow::duration(arrow::TimeUnit::NANO)),
                                                arrow::field("integer", arrow::int64())});
    auto timestamp_builder = std::make_shared<arrow::TimestampBuilder>(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
    auto duration_builder = std::make_shared<arrow::DurationBuilder>(arrow::duration(arrow::TimeUnit::NANO), arrow::default_memory_pool());
    auto integer_builder = std::make_shared<arrow::Int64Builder>();
    arrow::DenseUnionBuilder value_builder(arrow::default_memory_pool(), {timestamp_builder, duration_builder, integer_builder}, value_type);
    ASSERT_TRUE(value_builder.Append(0).ok()); ASSERT_TRUE(timestamp_builder->Append(10).ok());
    ASSERT_TRUE(value_builder.Append(0).ok()); ASSERT_TRUE(timestamp_builder->Append(20).ok());
    ASSERT_TRUE(value_builder.Append(1).ok()); ASSERT_TRUE(duration_builder->Append(10).ok());
    ASSERT_TRUE(value_builder.Append(1).ok()); ASSERT_TRUE(duration_builder->Append(20).ok());
    ASSERT_TRUE(value_builder.Append(2).ok()); ASSERT_TRUE(integer_builder->Append(10).ok());
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(value_builder.Finish(&value).ok());

    arrow::StringBuilder pv_builder;
    for (const auto& pv : {"TS:10", "TS:20", "D:10", "D:20", "I:10"}) ASSERT_TRUE(pv_builder.Append(pv).ok());
    std::shared_ptr<arrow::Array> pv;
    ASSERT_TRUE(pv_builder.Finish(&pv).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("value", value_type)}), 5, {pv, value});
    ASSERT_TRUE(catalog->create("typed_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner planner(catalog);
    query::QueryExecutor executor;
    const auto expect = [&](const std::string_view sql, const std::vector<std::string>& expected_pvs, const std::vector<arrow::Type::type>& active_types)
    {
        const auto result = executor.execute(planner.plan(query::parseQuery(sql)), context);
        EXPECT_EQ(result.stats.rpc_calls, 0U);
        ASSERT_EQ(result.batches.size(), 1U);
        const auto& selected = result.batches.front();
        ASSERT_EQ(selected->num_rows(), static_cast<int64_t>(expected_pvs.size()));
        ASSERT_EQ(active_types.size(), expected_pvs.size());
        if (expected_pvs.empty()) return;
        for (int64_t row = 0; row < selected->num_rows(); ++row)
        {
            const auto pv_scalar = selected->column(0)->GetScalar(row);
            const auto value_scalar = selected->column(1)->GetScalar(row);
            ASSERT_TRUE(pv_scalar.ok());
            ASSERT_TRUE(value_scalar.ok());
            EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringScalar>(*pv_scalar)->ToString(), expected_pvs[static_cast<std::size_t>(row)]);
            EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*value_scalar)->child_value()->type->id(), active_types[static_cast<std::size_t>(row)]);
        }
    };

    expect("SELECT * FROM typed_samples WHERE value = timestamp_ns(10)", {"TS:10"}, {arrow::Type::TIMESTAMP});
    expect("SELECT * FROM typed_samples WHERE value != timestamp_ns(10)", {"TS:20"}, {arrow::Type::TIMESTAMP});
    expect("SELECT * FROM typed_samples WHERE value IN (duration_ns(20), timestamp_ns(10))", {"TS:10", "D:20"}, {arrow::Type::TIMESTAMP, arrow::Type::DURATION});
    expect("SELECT * FROM typed_samples WHERE value BETWEEN timestamp_ns(10) AND timestamp_ns(20)", {"TS:10", "TS:20"}, {arrow::Type::TIMESTAMP, arrow::Type::TIMESTAMP});
    expect("SELECT * FROM typed_samples WHERE value > duration_ns(10)", {"D:20"}, {arrow::Type::DURATION});
    expect("SELECT * FROM typed_samples WHERE value BETWEEN duration_ns(10) AND timestamp_ns(20)", {}, {});
}

TEST_F(PlannerExecutorTest, PushesBackendPredicateAndPrunesProjectionColumns)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A'"));
    const auto*         scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates[0].column, "pv");
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv"}));
    EXPECT_EQ(query::plan::physicalPlanToString(plan).find("PhysicalFilter"), std::string::npos);
}

TEST_F(PlannerExecutorTest, FoldsToUtcPredicateToEpochSecondsBeforePushdown)
{
    query::QueryPlanner planner;
    const auto plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.samples WHERE pv = 'A' AND time >= to_utc('1970-01-01T00:00:10Z')"));
    const auto* scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 2U);

    const auto time = std::find_if(scan->pushable_predicates.begin(), scan->pushable_predicates.end(), [](const query::Predicate& predicate)
    {
        return predicate.column == "time";
    });
    ASSERT_NE(time, scan->pushable_predicates.end());
    ASSERT_EQ(time->op, query::PredicateOp::GTE);
    ASSERT_EQ(time->values.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<int64_t>(time->values.front()));
    EXPECT_EQ(std::get<int64_t>(time->values.front()), 10);
}

TEST_F(PlannerExecutorTest, RetainsFilterableOnlyPredicateForLocalExecution)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' AND value = 1"));
    const auto*         project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1);
    EXPECT_EQ(filter->predicates[0].column, "value");
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates[0].column, "pv");
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv", "value"}));
}

TEST_F(PlannerExecutorTest, ExecutesGenericPushableInSubqueryBeforeDependentScan)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.samples WHERE pv IN (SELECT pv FROM fake.meta WHERE pv = 'A')"));
    const auto* scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->in_subqueries.size(), 1U);
    EXPECT_TRUE(scan->in_subqueries.front().pushable);
    EXPECT_EQ(scan->in_subqueries.front().predicate.column, "pv");

    const auto result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1U);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
    EXPECT_EQ(result.stats.rpc_calls, 2U);
}

TEST_F(PlannerExecutorTest, AppliesGenericLocalInSubqueryAfterFetch)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.samples WHERE pv = 'A' AND value IN (SELECT value FROM fake.samples WHERE pv = 'A')"));
    const auto* scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->in_subqueries.size(), 1U);
    EXPECT_FALSE(scan->in_subqueries.front().pushable);

    const auto result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.stats.rpc_calls, 2U);
}

TEST_F(PlannerExecutorTest, RetainsPushedMetadataPredicatesForLocalVerification)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND tag IN ('sample', 'magnet')"));
    const auto*         project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1);
    EXPECT_EQ(filter->predicates.front().column, "tag");
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 2);
    EXPECT_TRUE(scan->projection_hint.contains("tags"));
}

TEST_F(PlannerExecutorTest, LikeIsCaseInsensitiveAndRemainsLocal)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE '%L_CE'"));
    const auto*         project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1);
    EXPECT_EQ(filter->predicates.front().op, query::PredicateOp::LIKE);
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates.front().column, "pv");

    query::QueryExecutor executor;
    const auto           result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
}

TEST_F(PlannerExecutorTest, LikeSupportsStarAndEscapedWildcards)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;

    const auto star_result = executor.execute(
        planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE 'A*E'")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(star_result.batches.front()->num_rows(), 1);
    EXPECT_EQ(star_result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");

    const auto escaped_result = executor.execute(
        planner.plan(query::parseQuery(R"(SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE 'alice\%')")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(escaped_result.batches.front()->num_rows(), 0);
}

TEST_F(PlannerExecutorTest, ReportsBinderAndTypeFailuresWithTypedErrors)
{
    query::QueryPlanner planner;
    for (const std::string_view sql : {
             "SELECT pv FROM fake.unknown WHERE pv = 'A'",
             "SELECT unknown FROM fake.samples WHERE pv = 'A'",
             "SELECT pv FROM fake.samples WHERE time = 'not-a-time'",
             "SELECT pv FROM fake.samples WHERE pv >= 'A'",
             "SELECT value FROM fake.samples"})
    {
        try
        {
            (void)planner.plan(query::parseQuery(sql));
            FAIL() << "Expected planning failure for " << sql;
        }
        catch (const query::plan::PlannerException& error)
        {
            EXPECT_TRUE(std::holds_alternative<query::plan::BindError>(error.error()) ||
                        std::holds_alternative<query::plan::TypeError>(error.error()) ||
                        std::holds_alternative<query::plan::PlanError>(error.error()));
        }
    }
}

TEST_F(PlannerExecutorTest, ExplainShortCircuitsToPlanRow)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("EXPLAIN SELECT pv FROM fake.samples WHERE pv = 'A'");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches[0]->num_rows(), 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "plan");
}

TEST_F(PlannerExecutorTest, ExecutesFilterProjectAndLimit)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' LIMIT 1");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.stats.rows_returned, 1);
    EXPECT_EQ(result.batches[0]->schema()->num_fields(), 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "pv");
}

TEST_F(PlannerExecutorTest, AccumulatesBackendPagesAndTracksEveryRpc)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery("SELECT pv FROM fake.paged"));
    const auto           result = executor.execute(plan, query::ExecutionContext{.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 2);
    EXPECT_EQ(result.stats.rpc_calls, 2);
    EXPECT_EQ(result.stats.rows_from_backend, 2);
    EXPECT_EQ(result.stats.rows_returned, 2);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringArray>(result.batches[0]->column(0))->GetString(0), "A");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringArray>(result.batches[1]->column(0))->GetString(0), "B");
}

TEST_F(PlannerExecutorTest, ShowTablesReturnsRegisteredTables)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("SHOW TABLES");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_GE(result.batches[0]->num_rows(), 1);
}

TEST_F(PlannerExecutorTest, DescribeUsesReadableTypesAndOperators)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(
        planner.plan(query::parseQuery("DESCRIBE fake.samples")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    const auto& batch = result.batches.front();
    EXPECT_EQ(batch->column(1)->GetScalar(0).ValueOrDie()->ToString(), "string");
    EXPECT_EQ(batch->column(4)->GetScalar(0).ValueOrDie()->ToString(), "=,IN");
    EXPECT_EQ(batch->column(1)->GetScalar(1).ValueOrDie()->ToString(), "timestamp");
    EXPECT_EQ(batch->column(4)->GetScalar(1).ValueOrDie()->ToString(), "<=,>=");
}

TEST_F(PlannerExecutorTest, ExecutesInnerJoinWithQualifiedColumns)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .join_batch_size = 100,
    };

    const auto statement = query::parseQuery(
        "SELECT s.pv, m.owner FROM fake.samples s INNER JOIN fake.meta m ON s.pv = m.pv WHERE s.pv IN ('A', 'B')");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.stats.rows_returned, 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "s.pv");
    EXPECT_EQ(result.batches[0]->schema()->field(1)->name(), "m.owner");
}

TEST_F(PlannerExecutorTest, OrdersByAnUnselectedColumnBeforeApplyingLimit)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv IN ('A', 'B') ORDER BY time DESC LIMIT 1"));
    const auto*          scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv", "time"}));

    const auto result = executor.execute(
        plan,
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "B");
}

TEST_F(PlannerExecutorTest, SelectsFiltersAndOrdersDynamicAttributeColumns)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;

    const auto filtered = executor.execute(
        planner.plan(query::parseQuery("SELECT pv, attributes.device_group FROM fake.meta WHERE pv IN ('A', 'C') AND attributes.device_group = 'RF'")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(filtered.batches.size(), 1);
    ASSERT_EQ(filtered.batches.front()->num_rows(), 1);
    EXPECT_EQ(filtered.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
    EXPECT_EQ(filtered.batches.front()->column(1)->GetScalar(0).ValueOrDie()->ToString(), "RF");

    const auto ordered = executor.execute(
        planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv IN ('A', 'C') ORDER BY attributes.device_group DESC LIMIT 1")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(ordered.batches.size(), 1);
    ASSERT_EQ(ordered.batches.front()->num_rows(), 1);
    EXPECT_EQ(ordered.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
}

TEST_F(PlannerExecutorTest, ExecutesLeftJoinWithNullRightSide)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .join_batch_size = 100,
    };

    const auto statement = query::parseQuery(
        "SELECT * FROM fake.samples s LEFT JOIN fake.meta m ON s.pv = m.pv WHERE s.pv IN ('A', 'B')");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.stats.rows_returned, 2);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "s.pv");
    EXPECT_EQ(result.batches[0]->schema()->field(3)->name(), "m.pv");
    EXPECT_EQ(result.batches[0]->schema()->field(4)->name(), "m.owner");
}

TEST_F(PlannerExecutorTest, RejectsAmbiguousUnqualifiedColumnAcrossJoin)
{
    query::QueryPlanner planner;
    const auto          statement = query::parseQuery("SELECT pv FROM fake.samples s JOIN fake.meta m ON s.pv = m.pv");
    EXPECT_THROW((void)planner.plan(statement), query::plan::PlannerException);
}

} // namespace
