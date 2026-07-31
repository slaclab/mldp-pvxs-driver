//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file RequiredColumnCheck.h
 * @brief Declares validation of required scan predicates. */
#pragma once

#include <query/plan/LogicalPlan.h>

namespace mldp_pvxs_driver::query::planner {

void requiredColumnCheck(const plan::LogicalNodePtr& root);

} // namespace mldp_pvxs_driver::query::planner
