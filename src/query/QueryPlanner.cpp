//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryPlanner.h>

#include <query/planner/Binder.h>
#include <query/planner/ColumnPruning.h>
#include <query/planner/CorrelatedPushOptimizer.h>
#include <query/planner/ConstantFolding.h>
#include <query/planner/JoinOrderOptimizer.h>
#include <query/planner/LogicalPlanner.h>
#include <query/planner/PhysicalPlanner.h>
#include <query/planner/PredicatePushdown.h>
#include <query/planner/RequiredColumnCheck.h>
#include <query/planner/TypeChecker.h>
#include <query/plan/PlannerError.h>
#include <query/QueryableFactory.h>
#include <query/QueryTableCatalog.h>

using namespace mldp_pvxs_driver::query;

plan::PhysicalNodePtr QueryPlanner::planUnion(const SelectStatement& select) const
{
    // Left-associative fold: consecutive UNION ALL branches share one node, and each
    // UNION (distinct) closes the rows accumulated so far into a deduplicating node.
    auto current = plan(QueryStatement{setOperationHead(select)});
    for (const auto& operand : select.set_operations)
    {
        if (!operand.query) throw plan::PlannerException(plan::PlanError{.message = "UNION branch is empty"});
        auto branch = plan(QueryStatement{*operand.query});
        auto* open_all = std::get_if<plan::PhysicalUnion>(&current->value);
        if (operand.all && open_all != nullptr && !open_all->distinct)
        {
            open_all->inputs.push_back(std::move(branch));
            continue;
        }
        current = plan::makeNode(plan::PhysicalUnion{.inputs = {std::move(current), std::move(branch)}, .distinct = !operand.all});
    }

    if (!select.order_by.empty())
    {
        std::vector<plan::SortKey> keys;
        for (const auto& item : select.order_by)
        {
            const QualifiedColumn* column = &item.column;
            if (item.expression)
            {
                column = std::get_if<QualifiedColumn>(&item.expression->value);
                if (column == nullptr)
                    throw plan::PlannerException(plan::PlanError{.message = "ORDER BY over a UNION accepts only output column names"});
            }
            if (column->name.empty()) throw plan::PlannerException(plan::PlanError{.message = "ORDER BY over a UNION accepts only output column names"});
            keys.push_back(plan::SortKey{.column = column->name, .expression = nullptr, .descending = item.direction == SortDirection::DESCENDING});
        }
        current = plan::makeNode(plan::PhysicalSort{.input = std::move(current), .keys = std::move(keys)});
    }
    if (select.limit)
    {
        current = plan::makeNode(plan::PhysicalLimit{.input = std::move(current), .limit = *select.limit});
    }
    planner::propagateScanRowLimit(current, std::nullopt);
    return current;
}

QueryPlanner::QueryPlanner(std::shared_ptr<QueryTableCatalog> catalog)
    : catalog_(std::move(catalog))
{
}

plan::PhysicalNodePtr QueryPlanner::plan(const QueryStatement& statement) const
{
    if (std::holds_alternative<ShowTablesStatement>(statement))
    {
        return plan::makeNode(plan::PhysicalShowTables{});
    }

    if (std::holds_alternative<ShowFunctionsStatement>(statement))
    {
        return plan::makeNode(plan::PhysicalShowFunctions{});
    }

    if (std::holds_alternative<ShowOperatorsStatement>(statement))
    {
        return plan::makeNode(plan::PhysicalShowOperators{});
    }

    if (std::holds_alternative<DescribeStatement>(statement))
    {
        const auto& describe = std::get<DescribeStatement>(statement);
        return plan::makeNode(plan::PhysicalDescribe{.table_name = describe.table_name});
    }

    if (std::holds_alternative<ExplainStatement>(statement))
    {
        const auto& explain = std::get<ExplainStatement>(statement);
        const auto explained_plan = plan(QueryStatement{explain.query});
        return plan::makeNode(plan::PhysicalExplain{
            .plan_text = plan::physicalPlanToString(explained_plan)});
    }

    if (std::holds_alternative<CreateTableStatement>(statement))
    {
        const auto& create = std::get<CreateTableStatement>(statement);
        return plan::makeNode(plan::PhysicalCreateTable{
            .table_name = create.table_name,
            .temporary = create.temporary,
            .query = plan(QueryStatement{create.query})});
    }

    if (std::holds_alternative<DropTableStatement>(statement))
    {
        return plan::makeNode(plan::PhysicalDropTable{.table_name = std::get<DropTableStatement>(statement).table_name});
    }

    if (!std::holds_alternative<SelectStatement>(statement))
    {
        throw plan::PlannerException(plan::PlanError{.message = "Unsupported statement"});
    }

    const auto& select = std::get<SelectStatement>(statement);
    if (!select.set_operations.empty())
    {
        return planUnion(select);
    }
    auto bound = planner::bindSelect(select, catalog_.get());
    bound = planner::typeCheckSelect(std::move(bound));
    auto logical = planner::buildLogicalPlan(bound);
    logical = planner::applyPredicatePushdown(std::move(logical));
    logical = planner::applyJoinOrderOptimizer(std::move(logical));
    logical = planner::applyConstantFolding(std::move(logical));
    logical = planner::applyColumnPruning(std::move(logical));
    planner::requiredColumnCheck(logical);
    auto physical = planner::buildPhysicalPlan(logical);
    physical = planner::applyCorrelatedPushOptimizer(std::move(physical));
    return physical;
}
