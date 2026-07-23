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

#include <query/IQueryable.h>

#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::query::plan {

struct PhysicalNode;
using PhysicalNodePtr = std::shared_ptr<PhysicalNode>;

struct PhysicalTableScan {
    std::string            table_name;
    std::vector<Predicate> pushable_predicates;
    std::set<std::string>  projection_hint;
};

struct PhysicalFilter {
    PhysicalNodePtr        input;
    std::vector<Predicate> predicates;
};

struct PhysicalProject {
    PhysicalNodePtr          input;
    std::vector<std::string> columns;
};

struct PhysicalLimit {
    PhysicalNodePtr input;
    uint64_t       limit{0};
};

struct PhysicalShowTables {
};

struct PhysicalDescribe {
    std::string table_name;
};

struct PhysicalExplain {
    std::string plan_text;
};

using PhysicalNodeVariant = std::variant<PhysicalTableScan,
                                         PhysicalFilter,
                                         PhysicalProject,
                                         PhysicalLimit,
                                         PhysicalShowTables,
                                         PhysicalDescribe,
                                         PhysicalExplain>;

struct PhysicalNode {
    PhysicalNodeVariant value;
};

inline PhysicalNodePtr makeNode(PhysicalNodeVariant value)
{
    return std::make_shared<PhysicalNode>(PhysicalNode{std::move(value)});
}

std::string physicalPlanToString(const PhysicalNodePtr& root);

} // namespace mldp_pvxs_driver::query::plan
