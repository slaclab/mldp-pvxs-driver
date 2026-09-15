//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file TypeChecker.h
 * @brief Declares semantic type checking for bound selections. */
#pragma once

#include <query/plan/LogicalPlan.h>

namespace mldp_pvxs_driver::query::planner {

/** @brief Validates expression types in a bound SELECT and resolves any remaining ambiguities.
 * @param[in] bound Bound SELECT to type-check.
 * @return The type-checked BoundSelect (field types may be updated).
 * @throws plan::PlannerException On type errors. */
plan::BoundSelect typeCheckSelect(plan::BoundSelect bound);

} // namespace mldp_pvxs_driver::query::planner
