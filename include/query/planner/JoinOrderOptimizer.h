//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file JoinOrderOptimizer.h
 * @brief Declares logical join-order optimization. */
#pragma once

#include <query/plan/LogicalPlan.h>

namespace mldp_pvxs_driver::query::planner {

/** @brief Reorders joins to reduce intermediate result sizes.
 * @param[in] root Logical plan root.
 * @return Optimized plan root (may be the same node). */
plan::LogicalNodePtr applyJoinOrderOptimizer(plan::LogicalNodePtr root);

} // namespace mldp_pvxs_driver::query::planner
