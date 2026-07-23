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

#include <config/Config.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/memory_pool.h>
#include <gtest/gtest.h>

#include <set>
#include <string_view>

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

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {
            {"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
            {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
            {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::EQ}, "value"},
        };
    }

    query::QueryResult execute(std::string_view,
                               const std::vector<query::Predicate>& pushable_predicates,
                               const std::set<std::string>& projection_hint,
                               const query::ExecutionContext&) override
    {
        arrow::StringBuilder pv_builder;
        arrow::Int64Builder time_builder;
        arrow::Int64Builder value_builder;

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

const std::set<std::string_view> FakeQueryable::kVirtualTables = {"fake.samples"};

class PlannerExecutorTest : public ::testing::Test {
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

TEST_F(PlannerExecutorTest, PlansSelectWithBackboneNodes)
{
    query::QueryPlanner planner;
    const auto statement = query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' LIMIT 1");
    const auto plan = planner.plan(statement);
    const auto text = query::plan::physicalPlanToString(plan);
    EXPECT_NE(text.find("PhysicalLimit"), std::string::npos);
    EXPECT_NE(text.find("PhysicalProject"), std::string::npos);
    EXPECT_NE(text.find("PhysicalTableScan"), std::string::npos);
}

TEST_F(PlannerExecutorTest, ExplainShortCircuitsToPlanRow)
{
    query::QueryPlanner planner;
    query::QueryExecutor executor;
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
    query::QueryPlanner planner;
    query::QueryExecutor executor;
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

TEST_F(PlannerExecutorTest, ShowTablesReturnsRegisteredTables)
{
    query::QueryPlanner planner;
    query::QueryExecutor executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("SHOW TABLES");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_GE(result.batches[0]->num_rows(), 1);
}

} // namespace
