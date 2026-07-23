//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/planner/TypeChecker.h>

#include <query/plan/PlannerError.h>

#include <chrono>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::planner;

namespace {

void enforceLiteralType(const plan::PlannerPredicate& predicate, const plan::PlannerLiteralValue& value)
{
    const auto invalid = [&predicate]() {
        throw plan::PlannerException(plan::TypeError{
            .message = "Type mismatch for column '" + predicate.column + "'"});
    };

    switch (predicate.column_type)
    {
    case ColumnType::STRING:
        if (!std::holds_alternative<std::string>(value))
        {
            invalid();
        }
        return;
    case ColumnType::TIMESTAMP:
    case ColumnType::DURATION_SECONDS:
    case ColumnType::INT:
        if (!std::holds_alternative<int64_t>(value))
        {
            invalid();
        }
        return;
    case ColumnType::BOOL:
        if (!std::holds_alternative<bool>(value))
        {
            invalid();
        }
        return;
    }
}

} // namespace

plan::BoundSelect mldp_pvxs_driver::query::planner::typeCheckSelect(plan::BoundSelect bound)
{
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

    const auto type_check_predicates = [&](std::vector<plan::PlannerPredicate>& predicates)
    {
        for (auto& predicate : predicates)
        {
            for (auto& value : predicate.values)
            {
                if (std::holds_alternative<NowLiteral>(value))
                {
                    if (predicate.column_type != ColumnType::TIMESTAMP)
                    {
                        throw plan::PlannerException(plan::TypeError{
                            .message = "NOW can only be compared against TIMESTAMP columns"});
                    }

                    const auto now = std::get<NowLiteral>(value);
                    value = static_cast<int64_t>(now_epoch + now.offset_seconds);
                }
                enforceLiteralType(predicate, value);
            }
        }
    };

    type_check_predicates(bound.from.predicates);
    for (auto& join : bound.joins)
    {
        type_check_predicates(join.table.predicates);
    }

    return bound;
}
