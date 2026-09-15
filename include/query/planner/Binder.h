//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file Binder.h
 * @brief Declares SQL binding against queryable schemas and catalog tables. */
#pragma once

#include <query/parser/QueryAST.h>
#include <query/plan/LogicalPlan.h>

namespace mldp_pvxs_driver::query {
class QueryTableCatalog;
}

namespace mldp_pvxs_driver::query::planner {

/** @brief Resolves column names and table schemas into a BoundSelect.
 * @param[in] statement Parsed SELECT statement to bind.
 * @param[in] catalog Optional catalog for user-created table resolution; null skips catalog lookup.
 * @return Bound SELECT.
 * @throws plan::PlannerException On binding errors (unknown columns, tables, etc.). */
plan::BoundSelect bindSelect(const SelectStatement& statement, const QueryTableCatalog* catalog = nullptr);

} // namespace mldp_pvxs_driver::query::planner
