//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file ColumnPredicateFilter.h
 * @brief Shared column_type / tag / attributes.KEY / provenance.KEY predicate
 *        matching for MLDP native columns, used by both the queryTable (wide)
 *        and queryDataBidiStream (long) execution paths. */
#pragma once

#include <query/IQueryable.h>
#include <query/impl/mldp/DataValueBuilder.h>

#include <common.pb.h>

#include <optional>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/** @brief True if a single string value matches a metadata-style predicate (=, IN, PREFIX, CONTAINS, LIKE). */
bool matchesStringPredicate(const mldp_pvxs_driver::query::Predicate& predicate, std::string_view value);

/** @brief Value-kind across a set of DataValues; nullopt if all unset, throws on mixed kinds within the same column. */
template <typename DataValueRange>
std::optional<std::string_view> dataValuesKind(const DataValueRange& values)
{
    std::optional<std::string_view> kind;
    for (const auto& value : values)
    {
        if (value.value_case() == dp::service::common::DataValue::VALUE_NOT_SET)
            continue;
        const auto candidate = dataValueKind(value);
        if (!kind)
            kind = candidate;
        else if (*kind != candidate)
            throw std::runtime_error("MLDP column contains mixed data types");
    }
    return kind;
}

/** @brief True if column-level metadata and value kind satisfy every column_type / tag / attributes.KEY / provenance.KEY predicate. */
bool matchesColumnMetadataPredicates(const dp::service::common::ColumnMetadata&              metadata,
                                     std::optional<std::string_view>                          column_type_kind,
                                     const std::vector<mldp_pvxs_driver::query::Predicate>&   predicates);

} // namespace mldp_pvxs_driver::query::impl::mldp
