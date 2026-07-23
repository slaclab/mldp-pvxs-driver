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
#include <query/plan/LogicalPlan.h>

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
    std::string            table_alias;
    bool                   qualify_output{false};
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

struct PhysicalSort {
    PhysicalNodePtr       input;
    std::vector<SortKey> keys;
};

enum class JoinType { INNER, LEFT_OUTER };
enum class JoinAlgorithm { HASH, NESTED_LOOP, BLOCK_NESTED_LOOP };

struct JoinCondition {
    std::string left_column;
    std::string right_column;
};

struct PhysicalHashJoin {
    JoinType                  type{JoinType::INNER};
    JoinCondition             condition;
    JoinAlgorithm             algorithm{JoinAlgorithm::HASH};
    PhysicalNodePtr           left;
    PhysicalNodePtr           right;
    std::vector<std::string>  warnings;
};

struct PhysicalNestedLoopJoin {
    JoinType        type{JoinType::INNER};
    JoinCondition   condition;
    JoinAlgorithm   algorithm{JoinAlgorithm::NESTED_LOOP};
    PhysicalNodePtr outer;
    PhysicalNodePtr inner;
    bool            correlated_push{false};
};

struct PhysicalBlockNestedLoopJoin {
    JoinType                 type{JoinType::INNER};
    JoinCondition            condition;
    JoinAlgorithm            algorithm{JoinAlgorithm::BLOCK_NESTED_LOOP};
    PhysicalNodePtr          outer;
    PhysicalNodePtr          inner;
    std::vector<std::string> warnings;
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
                                         PhysicalSort,
                                         PhysicalLimit,
                                         PhysicalHashJoin,
                                         PhysicalNestedLoopJoin,
                                         PhysicalBlockNestedLoopJoin,
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
