//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file PredicatePushdown.h
 * @brief Declares predicate pushdown from logical filters into scans. */
#pragma once

#include <query/plan/LogicalPlan.h>

namespace mldp_pvxs_driver::query::planner {

/** @brief Moves filter predicates as close to their source scans as possible.
 * @param[in] root Logical plan root.
 * @return Optimized plan root (may be the same node). */
plan::LogicalNodePtr applyPredicatePushdown(plan::LogicalNodePtr root);

} // namespace mldp_pvxs_driver::query::planner
