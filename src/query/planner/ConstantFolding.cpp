//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/ConstantFolding.h>

#include <sstream>
#include <type_traits>
#include <unordered_set>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

std::string literalKey(const plan::PlannerLiteralValue& value)
{
    return std::visit(
        [](const auto& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                return "s:" + v;
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return "i:" + std::to_string(v);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return "d:" + std::to_string(v);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return std::string("b:") + (v ? "1" : "0");
            }
            else
            {
                return "n:" + std::to_string(v.offset_seconds);
            }
        },
        value);
}

std::string predicateKey(const plan::PlannerPredicate& predicate)
{
    std::ostringstream out;
    out << predicate.column << "#" << static_cast<int>(predicate.op);
    for (const auto& value : predicate.values)
    {
        out << "#" << literalKey(value);
    }
    return out.str();
}

void dedupe(std::vector<plan::PlannerPredicate>& predicates)
{
    std::unordered_set<std::string> seen;
    std::vector<plan::PlannerPredicate> output;
    output.reserve(predicates.size());
    for (const auto& predicate : predicates)
    {
        const auto key = predicateKey(predicate);
        if (seen.insert(key).second)
        {
            output.push_back(predicate);
        }
    }
    predicates = std::move(output);
}

void rewrite(const plan::LogicalNodePtr& node)
{
    if (!node)
    {
        return;
    }
    if (auto* scan = std::get_if<plan::LogicalScan>(&node->value))
    {
        dedupe(scan->pushable_predicates);
        return;
    }
    if (auto* filter = std::get_if<plan::LogicalFilter>(&node->value))
    {
        dedupe(filter->predicates);
        rewrite(filter->input);
        return;
    }
    if (auto* project = std::get_if<plan::LogicalProject>(&node->value))
    {
        rewrite(project->input);
        return;
    }
    if (auto* limit = std::get_if<plan::LogicalLimit>(&node->value))
    {
        rewrite(limit->input);
        return;
    }
    if (auto* join = std::get_if<plan::LogicalJoin>(&node->value))
    {
        dedupe(join->predicates);
        rewrite(join->left);
        rewrite(join->right);
    }
}

} // namespace

plan::LogicalNodePtr mldp_pvxs_driver::query::planner::applyConstantFolding(plan::LogicalNodePtr root)
{
    rewrite(root);
    return root;
}
