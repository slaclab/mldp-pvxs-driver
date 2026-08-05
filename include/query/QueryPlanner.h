//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryPlanner.h
 * @brief Declares the SQL-to-physical-plan planning facade. */
#pragma once

#include <query/parser/QueryAST.h>
#include <query/plan/PhysicalPlan.h>

#include <memory>

namespace mldp_pvxs_driver::query {

class QueryTableCatalog;

/** @brief Binds, rewrites, and lowers parsed statements into physical plans. */
class QueryPlanner
{
public:
    /**
     * @brief Constructs a planner with an optional persistent table catalog.
     * @param[in] catalog Optional catalog for CREATE TABLE resolution; null uses schema-only planning.
     */
    explicit QueryPlanner(std::shared_ptr<QueryTableCatalog> catalog = nullptr);

    /**
     * @brief Lowers a parsed SQL statement to an executable physical plan.
     * @param[in] statement Parsed query statement.
     * @return Physical plan root.
     * @throws plan::PlannerException On bind, type, or planning errors.
     */
    plan::PhysicalNodePtr plan(const QueryStatement& statement) const;

private:
    std::shared_ptr<QueryTableCatalog> catalog_;
};

} // namespace mldp_pvxs_driver::query
