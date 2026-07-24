//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/parser/QueryAST.h>
#include <query/plan/PhysicalPlan.h>

#include <memory>

namespace mldp_pvxs_driver::query {

class QueryTableCatalog;

class QueryPlanner
{
public:
    explicit QueryPlanner(std::shared_ptr<QueryTableCatalog> catalog = nullptr);
    plan::PhysicalNodePtr plan(const QueryStatement& statement) const;

private:
    std::shared_ptr<QueryTableCatalog> catalog_;
};

} // namespace mldp_pvxs_driver::query
