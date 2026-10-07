//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file PlanVisit.h
 * @brief Generic traversal helpers for logical and physical plan trees.
 *
 * Plan walkers handle only the node types they act on and delegate every other
 * node to forEachChild(), which knows the children of each node type.  The
 * helpers list every node type explicitly (no catch-all), so adding a node to
 * LogicalNodeVariant or PhysicalNodeVariant fails to compile here until its
 * children are declared, instead of being silently skipped by some walker. */
#pragma once

#include <query/plan/LogicalPlan.h>
#include <query/plan/PhysicalPlan.h>

#include <variant>

namespace mldp_pvxs_driver::query::plan {

/** @brief Builds an overload set from lambdas for std::visit. */
template <class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/** @brief Calls @p fn with a reference to every child pointer of @p node, left to right.
 *
 *  Children are passed by reference so rewrite passes may replace them in place.
 * @param[in,out] node Logical node.
 * @param[in]     fn   Callable taking LogicalNodePtr&. */
template <typename Fn>
void forEachChild(LogicalNode& node, Fn&& fn)
{
    std::visit(overloaded{
                   [](LogicalScan&) {},
                   [&](LogicalFilter& value) { fn(value.input); },
                   [&](LogicalProject& value) { fn(value.input); },
                   [&](LogicalSort& value) { fn(value.input); },
                   [&](LogicalLimit& value) { fn(value.input); },
                   [&](LogicalAggregate& value) { fn(value.input); },
                   [&](LogicalWindow& value) { fn(value.input); },
                   [&](LogicalJoin& value) { fn(value.left); fn(value.right); },
               },
               node.value);
}

/** @brief Calls @p fn with a reference to every child pointer of @p node, left to right.
 *
 *  The source query of CREATE TABLE is a child; EXPLAIN, SHOW, DESCRIBE and
 *  DROP nodes have none.
 * @param[in,out] node Physical node.
 * @param[in]     fn   Callable taking PhysicalNodePtr&. */
template <typename Fn>
void forEachChild(PhysicalNode& node, Fn&& fn)
{
    std::visit(overloaded{
                   [](PhysicalTableScan&) {},
                   [&](PhysicalFilter& value) { fn(value.input); },
                   [&](PhysicalProject& value) { fn(value.input); },
                   [&](PhysicalSort& value) { fn(value.input); },
                   [&](PhysicalLimit& value) { fn(value.input); },
                   [&](PhysicalAggregate& value) { fn(value.input); },
                   [&](PhysicalWindow& value) { fn(value.input); },
                   [&](PhysicalPivot& value) { fn(value.input); },
                   [&](PhysicalHashJoin& value) { fn(value.left); fn(value.right); },
                   [&](PhysicalNestedLoopJoin& value) { fn(value.outer); fn(value.inner); },
                   [&](PhysicalBlockNestedLoopJoin& value) { fn(value.outer); fn(value.inner); },
                   [&](PhysicalCreateTable& value) { fn(value.query); },
                   [](PhysicalShowTables&) {},
                   [](PhysicalShowFunctions&) {},
                   [](PhysicalShowOperators&) {},
                   [](PhysicalDescribe&) {},
                   [](PhysicalExplain&) {},
                   [](PhysicalDropTable&) {},
               },
               node.value);
}

/** @brief True for join nodes, whose children produce alias-qualified columns. */
inline bool isJoin(const PhysicalNode& node) noexcept
{
    return std::holds_alternative<PhysicalHashJoin>(node.value) || std::holds_alternative<PhysicalNestedLoopJoin>(node.value) ||
           std::holds_alternative<PhysicalBlockNestedLoopJoin>(node.value);
}

} // namespace mldp_pvxs_driver::query::plan
