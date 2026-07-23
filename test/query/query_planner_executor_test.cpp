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
#include <query/QueryPlanner.h>
#include <query/QueryResult.h>
#include <query/QueryableFactory.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PlannerError.h>

#include <config/Config.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/memory_pool.h>
#include <arrow/scalar.h>
#include <gtest/gtest.h>

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
            };
        }

        return {
            {"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
            {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
            {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::EQ}, "value"},
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
            }
            if (include_row("C"))
            {
                EXPECT_TRUE(pv_builder.Append("C").ok());
                EXPECT_TRUE(owner_builder.Append("carol").ok());
            }

            std::shared_ptr<arrow::Array> pv;
            std::shared_ptr<arrow::Array> owner;
            EXPECT_TRUE(pv_builder.Finish(&pv).ok());
            EXPECT_TRUE(owner_builder.Finish(&owner).ok());

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

const std::set<std::string_view> FakeQueryable::kVirtualTables = {"fake.samples", "fake.meta", "fake.paged"};

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

TEST_F(PlannerExecutorTest, PushesBackendPredicateAndPrunesProjectionColumns)
{
    query::QueryPlanner planner;
    const auto plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A'"));
    const auto* scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates[0].column, "pv");
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv"}));
    EXPECT_EQ(query::plan::physicalPlanToString(plan).find("PhysicalFilter"), std::string::npos);
}

TEST_F(PlannerExecutorTest, RetainsFilterableOnlyPredicateForLocalExecution)
{
    query::QueryPlanner planner;
    const auto plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' AND value = 1"));
    const auto* project = std::get_if<plan::PhysicalProject>(&plan->value);
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

TEST_F(PlannerExecutorTest, LikeIsCaseInsensitiveAndRemainsLocal)
{
    query::QueryPlanner planner;
    const auto plan = planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE '%L_CE'"));
    const auto* project = std::get_if<plan::PhysicalProject>(&plan->value);
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
    const auto result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
}

TEST_F(PlannerExecutorTest, LikeSupportsStarAndEscapedWildcards)
{
    query::QueryPlanner planner;
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
