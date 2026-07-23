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
    auto root = plan::makeNode(plan::LogicalScan{
        .table_name = bound.table_name,
        .table_alias = bound.table_alias,
        .schema = bound.schema,
        .pushable_predicates = {},
        .projection_hint = {}});

    if (!bound.predicates.empty())
    {
        root = plan::makeNode(plan::LogicalFilter{
            .input = root,
            .predicates = bound.predicates});
    }

    if (!bound.select_all)
    {
        root = plan::makeNode(plan::LogicalProject{
            .input = root,
            .select_all = false,
            .columns = bound.select_columns});
    }

    if (bound.limit.has_value())
    {
        root = plan::makeNode(plan::LogicalLimit{
            .input = root,
            .limit = *bound.limit});
    }

    return root;
}
