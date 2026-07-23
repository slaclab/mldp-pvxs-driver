//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/LogicalPlanner.h>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::buildLogicalPlan(const plan::BoundSelect& bound)
{
    const bool qualify_output = !bound.joins.empty();

    const auto buildTableSubtree = [](const plan::BoundTable& table) -> plan::LogicalNodePtr
    {
        auto node = plan::makeNode(plan::LogicalScan{
            .table_name = table.table_name,
            .table_alias = table.table_alias,
            .schema = table.schema,
            .pushable_predicates = {},
            .projection_hint = {}});
        if (!table.predicates.empty())
        {
            node = plan::makeNode(plan::LogicalFilter{
                .input = node,
                .predicates = table.predicates});
        }
        return node;
    };

    auto root = buildTableSubtree(bound.from);
    for (const auto& join : bound.joins)
    {
        root = plan::makeNode(plan::LogicalJoin{
            .type = join.type,
            .condition = join.condition,
            .left = root,
            .right = buildTableSubtree(join.table),
            .predicates = {},
            .left_bounded = false,
            .right_bounded = false,
            .warnings = {}});
    }

    if (!bound.select_all)
    {
        root = plan::makeNode(plan::LogicalProject{
            .input = root,
            .select_all = false,
            .columns = bound.select_columns});
    }
    else if (qualify_output)
    {
        std::vector<std::string> columns;
        columns.reserve(bound.from.schema.size() + bound.joins.size() * 8);
        for (const auto& field : bound.from.schema)
        {
            if (field.is_output)
            {
                columns.push_back(bound.from.table_alias + "." + field.name);
            }
        }
        for (const auto& join : bound.joins)
        {
            for (const auto& field : join.table.schema)
            {
                if (field.is_output)
                {
                    columns.push_back(join.table.table_alias + "." + field.name);
                }
            }
        }
        root = plan::makeNode(plan::LogicalProject{
            .input = root,
            .select_all = true,
            .columns = std::move(columns)});
    }

    if (bound.limit.has_value())
    {
        root = plan::makeNode(plan::LogicalLimit{
            .input = root,
            .limit = *bound.limit});
    }

    return root;
}
