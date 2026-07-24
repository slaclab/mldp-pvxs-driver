//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/PhysicalPlanner.h>

#include <query/plan/PlannerError.h>

#include <sstream>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

std::vector<std::variant<std::string, int64_t, bool>>
toExecutableValues(const std::vector<plan::PlannerLiteralValue>& values)
{
    std::vector<std::variant<std::string, int64_t, bool>> converted;
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
        else
        {
            converted.push_back(std::get<bool>(value));
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

plan::PhysicalNodePtr buildNode(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return nullptr;
    }

    if (const auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        std::vector<Predicate> pushable_predicates;
        pushable_predicates.reserve(scan->pushable_predicates.size());
        for (const auto& predicate : scan->pushable_predicates)
        {
            pushable_predicates.push_back(toExecutablePredicate(predicate));
        }
        return plan::makeNode(plan::PhysicalTableScan{
            .table_name = scan->table_name,
            .table_alias = scan->table_alias,
            .qualify_output = false,
            .pushable_predicates = std::move(pushable_predicates),
            .projection_hint = scan->projection_hint,
            .ipc_path = scan->ipc_path,
            .arrow_ipc = scan->arrow_ipc,
            .derived_query = scan->derived_query});
    }
    if (const auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        std::vector<Predicate> predicates;
        predicates.reserve(filter->predicates.size());
        for (const auto& predicate : filter->predicates)
        {
            predicates.push_back(toExecutablePredicate(predicate));
        }
        return plan::makeNode(plan::PhysicalFilter{
            .input = buildNode(filter->input),
            .predicates = std::move(predicates)});
    }
    if (const auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        return plan::makeNode(plan::PhysicalProject{
            .input = buildNode(project->input),
            .columns = project->columns});
    }
    if (const auto* sort = std::get_if<plan::LogicalSort>(&node->value))
    {
        return plan::makeNode(plan::PhysicalSort{
            .input = buildNode(sort->input),
            .keys = sort->keys});
    }
    if (const auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        return plan::makeNode(plan::PhysicalLimit{
            .input = buildNode(limit->input),
            .limit = limit->limit});
    }
    if (const auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        return plan::makeNode(plan::PhysicalHashJoin{
            .type = join->type == plan::LogicalJoinType::LEFT_OUTER
                ? plan::JoinType::LEFT_OUTER
                : plan::JoinType::INNER,
            .condition = plan::JoinCondition{
                .left_column = join->condition.left_column,
                .right_column = join->condition.right_column},
            .algorithm = plan::JoinAlgorithm::HASH,
            .left = buildNode(join->left),
            .right = buildNode(join->right),
            .warnings = join->warnings});
    }
    return nullptr;
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
    if (auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        markJoinOutputQualification(filter->input, under_join);
        return;
    }
    if (auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        markJoinOutputQualification(project->input, under_join);
        return;
    }
    if (auto* sort = std::get_if<plan::PhysicalSort>(&node->value))
    {
        markJoinOutputQualification(sort->input, under_join);
        return;
    }
    if (auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        markJoinOutputQualification(limit->input, under_join);
        return;
    }
    if (auto* join = std::get_if<plan::PhysicalHashJoin>(&node->value))
    {
        markJoinOutputQualification(join->left, true);
        markJoinOutputQualification(join->right, true);
        return;
    }
    if (auto* join = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value))
    {
        markJoinOutputQualification(join->outer, true);
        markJoinOutputQualification(join->inner, true);
        return;
    }
    if (auto* join = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value))
    {
        markJoinOutputQualification(join->outer, true);
        markJoinOutputQualification(join->inner, true);
    }
}

std::string indent(const int level)
{
    return std::string(static_cast<size_t>(level) * 2, ' ');
}

void appendNode(std::ostringstream& out, const plan::PhysicalNodePtr& node, const int level)
{
    if (!node)
    {
        out << indent(level) << "<null>\n";
        return;
    }
    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        out << indent(level) << (scan->arrow_ipc ? "PhysicalArrowIpcScan(table=" : "PhysicalTableScan(table=") << scan->table_name << ")\n";
        return;
    }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        out << indent(level) << "PhysicalFilter(predicates=" << filter->predicates.size() << ")\n";
        appendNode(out, filter->input, level + 1);
        return;
    }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        out << indent(level) << "PhysicalProject(columns=" << project->columns.size() << ")\n";
        appendNode(out, project->input, level + 1);
        return;
    }
    if (const auto* sort = std::get_if<plan::PhysicalSort>(&node->value))
    {
        out << indent(level) << "PhysicalSort(keys=" << sort->keys.size() << ")\n";
        appendNode(out, sort->input, level + 1);
        return;
    }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        out << indent(level) << "PhysicalLimit(limit=" << limit->limit << ")\n";
        appendNode(out, limit->input, level + 1);
        return;
    }
    if (const auto* join = std::get_if<plan::PhysicalHashJoin>(&node->value))
    {
        out << indent(level) << "PhysicalHashJoin(type="
            << (join->type == plan::JoinType::LEFT_OUTER ? "LEFT" : "INNER")
            << ", algorithm=HASH"
            << ", on=" << join->condition.left_column << "=" << join->condition.right_column
            << ")\n";
        appendNode(out, join->left, level + 1);
        appendNode(out, join->right, level + 1);
        return;
    }
    if (const auto* join = std::get_if<plan::PhysicalNestedLoopJoin>(&node->value))
    {
        out << indent(level) << "PhysicalNestedLoopJoin(type="
            << (join->type == plan::JoinType::LEFT_OUTER ? "LEFT" : "INNER")
            << ", correlated_push=" << (join->correlated_push ? "true" : "false")
            << ", on=" << join->condition.left_column << "=" << join->condition.right_column
            << ")\n";
        appendNode(out, join->outer, level + 1);
        appendNode(out, join->inner, level + 1);
        return;
    }
    if (const auto* join = std::get_if<plan::PhysicalBlockNestedLoopJoin>(&node->value))
    {
        out << indent(level) << "PhysicalBlockNestedLoopJoin(type="
            << (join->type == plan::JoinType::LEFT_OUTER ? "LEFT" : "INNER")
            << ", on=" << join->condition.left_column << "=" << join->condition.right_column
            << ")\n";
        appendNode(out, join->outer, level + 1);
        appendNode(out, join->inner, level + 1);
        return;
    }
    if (std::holds_alternative<plan::PhysicalShowTables>(node->value))
    {
        out << indent(level) << "PhysicalShowTables\n";
        return;
    }
    if (const auto* describe = std::get_if<plan::PhysicalDescribe>(&node->value))
    {
        out << indent(level) << "PhysicalDescribe(table=" << describe->table_name << ")\n";
        return;
    }
    if (std::holds_alternative<plan::PhysicalExplain>(node->value))
    {
        out << indent(level) << "PhysicalExplain\n";
    }
    if (const auto* create = std::get_if<plan::PhysicalCreateTable>(&node->value))
    {
        out << indent(level) << "PhysicalCreateTable(table=" << create->table_name << ", lifetime=" << (create->temporary ? "session" : "persistent") << ")\n";
        appendNode(out, create->query, level + 1);
        return;
    }
    if (const auto* drop = std::get_if<plan::PhysicalDropTable>(&node->value))
    {
        out << indent(level) << "PhysicalDropTable(table=" << drop->table_name << ")\n";
    }
}

} // namespace

plan::PhysicalNodePtr mldp_pvxs_driver::query::planner::buildPhysicalPlan(const plan::LogicalNodePtr& root)
{
    auto physical = buildNode(root);
    markJoinOutputQualification(physical, false);
    return physical;
}

std::string mldp_pvxs_driver::query::plan::physicalPlanToString(const plan::PhysicalNodePtr& root)
{
    std::ostringstream out;
    if (!root)
    {
        out << "<empty>";
        return out.str();
    }

    if (std::holds_alternative<PhysicalExplain>(root->value))
    {
        out << std::get<PhysicalExplain>(root->value).plan_text;
        return out.str();
    }

    // Reuse planner formatter implemented in this translation unit.
    auto append = [&out](const PhysicalNodePtr& node, const auto& append_ref, int level) -> void
    {
        if (!node)
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ') << "<null>\n";
            return;
        }
        if (const auto* scan = std::get_if<PhysicalTableScan>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << (scan->arrow_ipc ? "PhysicalArrowIpcScan(table=" : "PhysicalTableScan(table=") << scan->table_name << ")\n";
            return;
        }
        if (const auto* filter = std::get_if<PhysicalFilter>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalFilter(predicates=" << filter->predicates.size() << ")\n";
            append_ref(filter->input, append_ref, level + 1);
            return;
        }
        if (const auto* project = std::get_if<PhysicalProject>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalProject(columns=" << project->columns.size() << ")\n";
            append_ref(project->input, append_ref, level + 1);
            return;
        }
        if (const auto* sort = std::get_if<PhysicalSort>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalSort(keys=" << sort->keys.size() << ")\n";
            append_ref(sort->input, append_ref, level + 1);
            return;
        }
        if (const auto* limit = std::get_if<PhysicalLimit>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalLimit(limit=" << limit->limit << ")\n";
            append_ref(limit->input, append_ref, level + 1);
            return;
        }
        if (const auto* join = std::get_if<PhysicalHashJoin>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalHashJoin(type=" << (join->type == JoinType::LEFT_OUTER ? "LEFT" : "INNER")
                << ", on=" << join->condition.left_column << "=" << join->condition.right_column << ")\n";
            append_ref(join->left, append_ref, level + 1);
            append_ref(join->right, append_ref, level + 1);
            return;
        }
        if (const auto* join = std::get_if<PhysicalNestedLoopJoin>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalNestedLoopJoin(type=" << (join->type == JoinType::LEFT_OUTER ? "LEFT" : "INNER")
                << ", correlated_push=" << (join->correlated_push ? "true" : "false")
                << ", on=" << join->condition.left_column << "=" << join->condition.right_column << ")\n";
            append_ref(join->outer, append_ref, level + 1);
            append_ref(join->inner, append_ref, level + 1);
            return;
        }
        if (const auto* join = std::get_if<PhysicalBlockNestedLoopJoin>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalBlockNestedLoopJoin(type=" << (join->type == JoinType::LEFT_OUTER ? "LEFT" : "INNER")
                << ", on=" << join->condition.left_column << "=" << join->condition.right_column << ")\n";
            append_ref(join->outer, append_ref, level + 1);
            append_ref(join->inner, append_ref, level + 1);
            return;
        }
        if (std::holds_alternative<PhysicalShowTables>(node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ') << "PhysicalShowTables\n";
            return;
        }
        if (const auto* describe = std::get_if<PhysicalDescribe>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ')
                << "PhysicalDescribe(table=" << describe->table_name << ")\n";
            return;
        }
        if (const auto* create = std::get_if<PhysicalCreateTable>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ') << "PhysicalCreateTable(table=" << create->table_name << ")\n";
            append_ref(create->query, append_ref, level + 1);
            return;
        }
        if (const auto* drop = std::get_if<PhysicalDropTable>(&node->value))
        {
            out << std::string(static_cast<size_t>(level) * 2, ' ') << "PhysicalDropTable(table=" << drop->table_name << ")\n";
            return;
        }
        out << std::string(static_cast<size_t>(level) * 2, ' ') << "PhysicalExplain\n";
    };

    append(root, append, 0);
    return out.str();
}
