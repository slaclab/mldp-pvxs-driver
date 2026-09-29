//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file PhysicalPlanner.h
 * @brief Declares conversion of logical plans to executable physical plans. */
#pragma once

#include <query/plan/LogicalPlan.h>
#include <query/plan/PhysicalPlan.h>

#include <cstdint>
#include <optional>

namespace mldp_pvxs_driver::query::planner {

/** @brief Lowers a logical plan tree to an executable physical plan.
 * @param[in] root Logical plan root node.
 * @return Physical plan root node. */
plan::PhysicalNodePtr buildPhysicalPlan(const plan::LogicalNodePtr& root);

/** @brief Rewrites the backend row limit carried by every scan under @p root.
 *
 * The budget descends only through cardinality-preserving nodes (LIMIT, projection);
 * any other node resets it. Scans reached with no budget have their row limit cleared,
 * so passing std::nullopt undoes a limit pushed by an earlier run of this pass.
 *
 * @param[in] root   Physical plan root node; may be null.
 * @param[in] budget Initial row budget, or std::nullopt to clear. */
void propagateScanRowLimit(const plan::PhysicalNodePtr& root, std::optional<uint64_t> budget);

} // namespace mldp_pvxs_driver::query::planner
