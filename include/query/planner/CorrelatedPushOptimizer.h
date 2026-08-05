//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file CorrelatedPushOptimizer.h
 * @brief Declares correlated-predicate push optimization for physical plans. */
#pragma once

#include <query/plan/PhysicalPlan.h>

namespace mldp_pvxs_driver::query::planner {

/** @brief Pushes correlated predicates from nested-loop joins into inner scans.
 * @param[in] root Physical plan root.
 * @return Optimized plan root (may be the same node). */
plan::PhysicalNodePtr applyCorrelatedPushOptimizer(plan::PhysicalNodePtr root);

} // namespace mldp_pvxs_driver::query::planner
