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

namespace mldp_pvxs_driver::query::planner {

/** @brief Lowers a logical plan tree to an executable physical plan.
 * @param[in] root Logical plan root node.
 * @return Physical plan root node. */
plan::PhysicalNodePtr buildPhysicalPlan(const plan::LogicalNodePtr& root);

} // namespace mldp_pvxs_driver::query::planner
