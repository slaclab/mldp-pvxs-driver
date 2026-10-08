//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/plan/PlanVisit.h>
#include <query/planner/PhysicalPlanner.h>

#include <query/plan/PlannerError.h>

#include <algorithm>
#include <optional>
#include <sstream>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

std::vector<ExecutableLiteralValue>
toExecutableValues(const std::vector<plan::PlannerLiteralValue>& values)
{
    std::vector<ExecutableLiteralValue> converted;
    converted.reserve(values.size());
    for (const auto& value : values)
    {
        if (std::holds_alternative<NowLiteral>(value))
        {
            throw plan::PlannerException(plan::TypeError{
                .message = "NOW literal reached physical planning unexpectedly"});
        }
        if (std::holds_alternative<std::string>(value))
        {
            converted.push_back(std::get<std::string>(value));
        }
        else if (std::holds_alternative<int64_t>(value))
        {
            converted.push_back(std::get<int64_t>(value));
        }
        else if (std::holds_alternative<double>(value))
        {
            converted.push_back(std::get<double>(value));
        }
        else if (std::holds_alternative<bool>(value))
        {
            converted.push_back(std::get<bool>(value));
        }
        else if (std::holds_alternative<TimestampNsLiteral>(value))
        {
            converted.push_back(std::get<TimestampNsLiteral>(value));
        }
        else
        {
            converted.push_back(std::get<DurationNsLiteral>(value));
        }
    }
    return converted;
}

Predicate toExecutablePredicate(const plan::PlannerPredicate& predicate)
{
    return Predicate{
        .column = predicate.column,
        .op = predicate.op,
        .values = toExecutableValues(predicate.values)};
}

plan::PhysicalNodePtr buildNode(const plan::LogicalNodePtr& node);

// mldp.time_series_table scans become a long-form scan pivoted to wide columns.
plan::PhysicalNodePtr buildScan(const plan::LogicalScan& scan)
{
    std::vector<Predicate> pushable_predicates;
    pushable_predicates.reserve(scan.pushable_predicates.size());
    for (const auto& predicate : scan.pushable_predicates)
    {
        pushable_predicates.push_back(toExecutablePredicate(predicate));
    }
    std::vector<plan::PhysicalInSubquery> in_subqueries;
    in_subqueries.reserve(scan.in_subqueries.size());
    for (const auto& subquery : scan.in_subqueries)
    {
        in_subqueries.push_back(plan::PhysicalInSubquery{
            .predicate = toExecutablePredicate(subquery.predicate),
            .column_type = subquery.predicate.column_type,
            .pushable = subquery.predicate.pushable_ops.contains(PredicateOp::IN),
            .child = subquery.child});
    }
    auto physical_scan = plan::makeNode(plan::PhysicalTableScan{
        .table_name = scan.table_name,
        .table_alias = scan.table_alias,
        .qualify_output = false,
        .pushable_predicates = std::move(pushable_predicates),
        .projection_hint = scan.projection_hint,
        .ipc_path = scan.ipc_path,
        .arrow_ipc = scan.arrow_ipc,
        .derived_query = scan.derived_query,
        .in_subqueries = std::move(in_subqueries),
        .window_subquery = scan.window_subquery,
        .window_literal = scan.window_literal ? std::optional<std::array<int64_t, 2>>{
            {std::get<int64_t>((*scan.window_literal)[0]), std::get<int64_t>((*scan.window_literal)[1])}} : std::nullopt,
        .window_shards = scan.window_shards,
        .projection_explicit = scan.projection_explicit});
    if (scan.table_name != "mldp.time_series_table") return physical_scan;
    // Configuration and sample-status selectors are exact only on the
    // sample-oriented backend query, so such scans keep the native wide path
    // instead of pivoting a bucket scan.
    const bool sample_selector = std::any_of(scan.pushable_predicates.begin(), scan.pushable_predicates.end(), [](const auto& predicate)
                                             { return predicate.column.rfind("status_", 0) == 0 || predicate.column.rfind("config_", 0) == 0; });
    if (sample_selector) return physical_scan;

    std::vector<std::string> output_column_labels;
    for (const auto& predicate : scan.pushable_predicates)
    {
        if (predicate.column != "pv" || (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)) continue;
        for (const auto& value : predicate.values)
            if (std::holds_alternative<std::string>(value)) output_column_labels.push_back(std::get<std::string>(value));
    }
    auto* long_scan = std::get_if<plan::PhysicalTableScan>(&physical_scan->value);
    long_scan->table_name = "mldp.time_series";
    return plan::makeNode(plan::PhysicalPivot{.input = std::move(physical_scan),
                                              .row_key_column = "time",
                                              .pivot_key_column = "pv",
                                              .value_column = "value",
                                              .output_column_labels = std::move(output_column_labels)});
}

bool isColumn(const ExpressionPtr& expression, const std::string_view name)
{
    const auto* column = expression ? std::get_if<QualifiedColumn>(&expression->value) : nullptr;
    return column != nullptr && column->name == name;
}

/** True when @p window's first group is `PARTITION BY pv ORDER BY time [ASC]` directly over an
 *  mldp.time_series scan, whose samples arrive in time order within each PV.  The executor still
 *  verifies the order and falls back to a full sort when it does not hold. */
bool timeSeriesSortedInput(const plan::LogicalWindow& window)
{
    if (window.groups.empty()) return false;
    const auto& group = window.groups.front();
    if (group.partition_by.size() != 1 || !isColumn(group.partition_by.front(), "pv")) return false;
    if (group.order_by.size() != 1 || group.order_by.front().descending || !isColumn(group.order_by.front().expression, "time")) return false;
    auto node = window.input;
    while (node)
    {
        if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value)) return scan->table_name == "mldp.time_series";
        if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value)) node = filter->input;
        else return false;
    }
    return false;
}

plan::PhysicalNodePtr buildNode(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return nullptr;
    }
    // Every logical node type is listed: a new one must say how it lowers.
    return std::visit(plan::overloaded{
                          [](const plan::LogicalScan& scan) { return buildScan(scan); },
                          [](const plan::LogicalFilter& filter)
                          {
                              std::vector<Predicate> predicates;
                              predicates.reserve(filter.predicates.size());
                              for (const auto& predicate : filter.predicates)
                              {
                                  predicates.push_back(toExecutablePredicate(predicate));
                              }
                              const auto to_group = [](const auto& self, const plan::PlannerPredicateGroup& group) -> PredicateGroup
                              {
                                  PredicateGroup result{.kind = group.kind, .leaf = {}, .children = {}};
                                  if (group.kind == PredicateGroup::Kind::LEAF) result.leaf = toExecutablePredicate(group.leaf);
                                  for (const auto& child : group.children) result.children.push_back(self(self, child));
                                  return result;
                              };
                              std::vector<PredicateGroup> groups;
                              for (const auto& group : filter.predicate_groups) groups.push_back(to_group(to_group, group));
                              return plan::makeNode(plan::PhysicalFilter{.input = buildNode(filter.input), .predicates = std::move(predicates), .predicate_groups = std::move(groups), .conditions = filter.conditions});
                          },
                          [](const plan::LogicalProject& project)
                          {
                              return plan::makeNode(plan::PhysicalProject{
                                  .input = buildNode(project.input),
                                  .columns = project.columns,
                                  .expressions = project.expressions,
                                  .names = project.names,
                                  .distinct = project.distinct,
                                  .distinct_on = project.distinct_on});
                          },
                          [](const plan::LogicalAggregate& aggregate)
                          {
                              return plan::makeNode(plan::PhysicalAggregate{.input = buildNode(aggregate.input), .spec = aggregate.spec});
                          },
                          [](const plan::LogicalWindow& window)
                          {
                              return plan::makeNode(plan::PhysicalWindow{.input = buildNode(window.input),
                                                                         .groups = window.groups,
                                                                         .sorted_input = timeSeriesSortedInput(window)});
                          },
                          [](const plan::LogicalSort& sort)
                          {
                              return plan::makeNode(plan::PhysicalSort{.input = buildNode(sort.input), .keys = sort.keys});
                          },
                          [](const plan::LogicalLimit& limit)
                          {
                              return plan::makeNode(plan::PhysicalLimit{.input = buildNode(limit.input), .limit = limit.limit});
                          },
                          [](const plan::LogicalJoin& join)
                          {
                              return plan::makeNode(plan::PhysicalHashJoin{
                                  .type = join.type == plan::LogicalJoinType::LEFT_OUTER ? plan::JoinType::LEFT_OUTER : plan::JoinType::INNER,
                                  .condition = plan::JoinCondition{.left_column = join.condition.left_column, .right_column = join.condition.right_column},
                                  .algorithm = plan::JoinAlgorithm::HASH,
                                  .left = buildNode(join.left),
                                  .right = buildNode(join.right),
                                  .warnings = join.warnings});
                          },
                      },
                      node->value);
}

void markJoinOutputQualification(const plan::PhysicalNodePtr& node, const bool under_join)
{
    if (!node)
    {
        return;
    }
    if (auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        scan->qualify_output = under_join;
        return;
    }
    // CREATE TABLE sources are planned on their own.
    if (std::holds_alternative<plan::PhysicalCreateTable>(node->value))
    {
        return;
    }
    const bool child_under_join = under_join || plan::isJoin(*node);
    plan::forEachChild(*node, [child_under_join](plan::PhysicalNodePtr& child) { markJoinOutputQualification(child, child_under_join); });
}

/** @brief Propagates a LIMIT row budget down to backend scans through cardinality-preserving nodes.
 *
 *  A budget only survives nodes that neither add nor drop rows.  Filters, sorts, joins,
 *  pivots and statement nodes reset it, so a residual post-fetch predicate (see
 *  PredicatePushdown) always suppresses the pushdown and SQL semantics are preserved.
 */
void propagateScanRowLimitImpl(const plan::PhysicalNodePtr& node, std::optional<uint64_t> budget);

void resetBudgetBelow(plan::PhysicalNode& node)
{
    plan::forEachChild(node, [](plan::PhysicalNodePtr& child) { propagateScanRowLimitImpl(child, std::nullopt); });
}

void propagateScanRowLimitImpl(const plan::PhysicalNodePtr& node, const std::optional<uint64_t> budget)
{
    if (!node)
    {
        return;
    }
    // Every node type is listed: a new one must decide how it treats the budget.
    std::visit(plan::overloaded{
                   [budget](plan::PhysicalTableScan& scan)
                   {
                       const bool plain = !scan.arrow_ipc && !scan.derived_query && scan.in_subqueries.empty() &&
                                          !scan.window_subquery && !scan.window_literal;
                       // Assign unconditionally so that re-running the pass with an empty budget
                       // clears a previously pushed limit.
                       scan.row_limit = budget && plain ? *budget : 0;
                   },
                   [budget](plan::PhysicalLimit& limit)
                   {
                       propagateScanRowLimitImpl(limit.input, budget ? std::min(*budget, limit.limit) : limit.limit);
                   },
                   [budget](plan::PhysicalProject& project)
                   {
                       // Projection never changes cardinality, so the budget passes through unchanged.
                       // DISTINCT drops rows, so the scan must not stop after `budget` input rows.
                       propagateScanRowLimitImpl(project.input, project.distinct ? std::nullopt : budget);
                   },
                   // Filters, sorts, aggregates (which read every row), pivots, joins and
                   // CREATE TABLE sources reset the budget.
                   [&node](plan::PhysicalFilter&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalSort&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalAggregate&) { resetBudgetBelow(*node); },
                   // A window reads whole partitions, so a LIMIT above it cannot stop the scan early.
                   [&node](plan::PhysicalWindow&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalPivot&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalHashJoin&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalNestedLoopJoin&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalBlockNestedLoopJoin&) { resetBudgetBelow(*node); },
                   [&node](plan::PhysicalCreateTable&) { resetBudgetBelow(*node); },
                   [](plan::PhysicalShowTables&) {},
                   [](plan::PhysicalShowFunctions&) {},
                   [](plan::PhysicalShowOperators&) {},
                   [](plan::PhysicalDescribe&) {},
                   [](plan::PhysicalExplain&) {},
                   [](plan::PhysicalDropTable&) {},
               },
               node->value);
}

} // namespace

void mldp_pvxs_driver::query::planner::propagateScanRowLimit(const plan::PhysicalNodePtr&  root,
                                                             const std::optional<uint64_t> budget)
{
    propagateScanRowLimitImpl(root, budget);
}

plan::PhysicalNodePtr mldp_pvxs_driver::query::planner::buildPhysicalPlan(const plan::LogicalNodePtr& root)
{
    auto physical = buildNode(root);
    markJoinOutputQualification(physical, false);
    propagateScanRowLimit(physical, std::nullopt);
    return physical;
}

namespace {

const char* joinTypeName(const mldp_pvxs_driver::query::plan::JoinType type)
{
    return type == mldp_pvxs_driver::query::plan::JoinType::LEFT_OUTER ? "LEFT" : "INNER";
}

void appendPlan(std::ostringstream& out, const mldp_pvxs_driver::query::plan::PhysicalNodePtr& node, const int level)
{
    using namespace mldp_pvxs_driver::query::plan;
    const std::string pad(static_cast<std::size_t>(level) * 2, ' ');
    if (!node)
    {
        out << pad << "<null>\n";
        return;
    }
    out << pad;
    // Every node type is listed so EXPLAIN never silently omits one.
    std::visit(overloaded{
                   [&](const PhysicalTableScan& scan)
                   {
                       out << (scan.arrow_ipc ? "PhysicalArrowIpcScan(table=" : "PhysicalTableScan(table=") << scan.table_name;
                       if (!scan.in_subqueries.empty() || scan.window_subquery || scan.window_literal)
                       {
                           out << ", in_subqueries=" << scan.in_subqueries.size()
                               << ", window_subquery=" << (scan.window_subquery ? "true" : "false")
                               << ", window_literal=" << (scan.window_literal ? "true" : "false");
                       }
                       if (scan.row_limit != 0) out << ", row_limit=" << scan.row_limit;
                       out << ")";
                   },
                   [&](const PhysicalFilter& filter) { out << "PhysicalFilter(predicates=" << filter.predicates.size();
                       if (!filter.predicate_groups.empty()) out << ", local_or_groups=" << filter.predicate_groups.size();
                       if (!filter.conditions.empty()) out << ", conditions=" << filter.conditions.size();
                       out << ")"; },
                   [&](const PhysicalProject& project)
                   {
                       out << "PhysicalProject(columns=" << project.columns.size() << (project.distinct ? ", distinct=true" : "");
                       if (!project.distinct_on.empty()) out << ", distinct_on=" << project.distinct_on.size();
                       out << ")";
                   },
                   [&](const PhysicalAggregate& aggregate)
                   {
                       out << "PhysicalAggregate(keys=" << aggregate.spec.keys.size() << ", aggregates=" << aggregate.spec.aggregates.size()
                           << (aggregate.spec.having ? ", having=true" : "") << ")";
                   },
                   [&](const PhysicalWindow& window)
                   {
                       std::size_t calls = 0;
                       for (const auto& group : window.groups) calls += group.calls.size();
                       out << "PhysicalWindow(groups=" << window.groups.size() << ", calls=" << calls;
                       if (!window.groups.empty())
                       {
                           out << ", partition=" << window.groups.front().partition_by.size()
                               << ", order=" << window.groups.front().order_by.size();
                       }
                       out << ", sorted_input=" << (window.sorted_input ? "true" : "false") << ")";
                   },
                   [&](const PhysicalSort& sort) { out << "PhysicalSort(keys=" << sort.keys.size() << ")"; },
                   [&](const PhysicalLimit& limit) { out << "PhysicalLimit(limit=" << limit.limit << ")"; },
                   [&](const PhysicalPivot& pivot)
                   {
                       out << "PhysicalPivot(columns=" << pivot.output_column_labels.size() << ", batch_size=" << pivot.output_batch_size << ")";
                   },
                   [&](const PhysicalHashJoin& join)
                   {
                       out << "PhysicalHashJoin(type=" << joinTypeName(join.type) << ", on=" << join.condition.left_column << "="
                           << join.condition.right_column << ")";
                   },
                   [&](const PhysicalNestedLoopJoin& join)
                   {
                       out << "PhysicalNestedLoopJoin(type=" << joinTypeName(join.type) << ", correlated_push=" << (join.correlated_push ? "true" : "false")
                           << ", on=" << join.condition.left_column << "=" << join.condition.right_column << ")";
                   },
                   [&](const PhysicalBlockNestedLoopJoin& join)
                   {
                       out << "PhysicalBlockNestedLoopJoin(type=" << joinTypeName(join.type) << ", on=" << join.condition.left_column << "="
                           << join.condition.right_column << ")";
                   },
                   [&](const PhysicalShowTables&) { out << "PhysicalShowTables"; },
                   [&](const PhysicalShowFunctions&) { out << "PhysicalShowFunctions"; },
                   [&](const PhysicalShowOperators&) { out << "PhysicalShowOperators"; },
                   [&](const PhysicalDescribe& describe) { out << "PhysicalDescribe(table=" << describe.table_name << ")"; },
                   [&](const PhysicalExplain&) { out << "PhysicalExplain"; },
                   [&](const PhysicalCreateTable& create) { out << "PhysicalCreateTable(table=" << create.table_name << ")"; },
                   [&](const PhysicalDropTable& drop) { out << "PhysicalDropTable(table=" << drop.table_name << ")"; },
               },
               node->value);
    out << "\n";
    forEachChild(*node, [&out, level](PhysicalNodePtr& child) { appendPlan(out, child, level + 1); });
}

} // namespace

std::string mldp_pvxs_driver::query::plan::physicalPlanToString(const plan::PhysicalNodePtr& root)
{
    if (!root)
    {
        return "<empty>";
    }
    if (const auto* explain = std::get_if<PhysicalExplain>(&root->value))
    {
        return explain->plan_text;
    }
    std::ostringstream out;
    appendPlan(out, root, 0);
    return out.str();
}
