//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/ColumnPredicateFilter.h>

#include <query/executor/ExecutorUtils.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query::impl::mldp;
using namespace mldp_pvxs_driver::query;

bool mldp_pvxs_driver::query::impl::mldp::matchesStringPredicate(const Predicate& predicate, const std::string_view value)
{
    if (predicate.op == PredicateOp::PREFIX || predicate.op == PredicateOp::CONTAINS || predicate.op == PredicateOp::LIKE)
    {
        if (predicate.values.size() != 1 || !std::holds_alternative<std::string>(predicate.values.front()))
            throw std::invalid_argument("MLDP metadata predicate '" + predicate.column + "' requires one string value");
        const auto& pattern = std::get<std::string>(predicate.values.front());
        if (predicate.op == PredicateOp::PREFIX)
            return value.rfind(pattern, 0) == 0;
        if (predicate.op == PredicateOp::CONTAINS)
            return value.find(pattern) != std::string_view::npos;
        return executor::matchesLikePattern(value, pattern);
    }
    if (predicate.op != PredicateOp::EQ && predicate.op != PredicateOp::IN)
        throw std::invalid_argument("MLDP metadata predicate '" + predicate.column + "' requires =, IN, PREFIX, CONTAINS, or LIKE");
    for (const auto& candidate : predicate.values)
    {
        if (!std::holds_alternative<std::string>(candidate))
            throw std::invalid_argument("MLDP metadata predicate '" + predicate.column + "' requires string values");
        if (std::get<std::string>(candidate) == value)
            return true;
    }
    return false;
}

bool mldp_pvxs_driver::query::impl::mldp::matchesColumnMetadataPredicates(
    const dp::service::common::ColumnMetadata& metadata,
    const std::optional<std::string_view>      column_type_kind,
    const std::vector<Predicate>&               predicates)
{
    for (const auto& predicate : predicates)
    {
        if (predicate.column == "column_type")
        {
            if (!column_type_kind || !matchesStringPredicate(predicate, *column_type_kind))
                return false;
        }
        else if (predicate.column == "tag")
        {
            bool matched = false;
            for (const auto& tag : metadata.tags())
            {
                if (matchesStringPredicate(predicate, tag))
                {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        else if (predicate.column.rfind("attributes.", 0) == 0)
        {
            const auto key = predicate.column.substr(std::string("attributes.").size());
            bool       matched = false;
            for (const auto& attribute : metadata.attributes())
            {
                if (attribute.name() == key && matchesStringPredicate(predicate, attribute.value()))
                {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        else if (predicate.column.rfind("provenance.", 0) == 0)
        {
            const auto        key = predicate.column.substr(std::string("provenance.").size());
            const auto&       provenance = metadata.provenance();
            const std::string value = key == "source" ? provenance.source() : key == "process" ? provenance.process()
                                                                                                 : "";
            if (value.empty() || !matchesStringPredicate(predicate, value))
                return false;
        }
    }
    return true;
}
