//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file AnnotationMetadataBuilders.h
 * @brief Accumulates Arrow metadata columns for MLDP annotation record batches. */
#pragma once

#include <query/impl/mldp/MetadataArrayBuilders.h>

#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/** @brief Builds tags, attributes map, and per-key scalar Arrow arrays for annotation records.
 *
 *  The @c append method is templated over the protobuf Metadata type; it requires the type
 *  to expose @c tags() and @c attributes() ranges matching the MLDP annotation proto layout.
 */
class AnnotationMetadataBuilders : public MetadataArrayBuilders
{
public:
    explicit AnnotationMetadataBuilders(const std::set<std::string>& attribute_keys);

    template <typename Metadata>
    void append(const Metadata& metadata)
    {
        beginTagsAndAttributes();
        for (const auto& tag : metadata.tags())
            appendTag(tag);
        std::unordered_map<std::string, std::string> seen_attributes;
        for (const auto& attribute : metadata.attributes())
        {
            seen_attributes.insert_or_assign(attribute.name(), attribute.value());
            appendAttributeEntry(attribute.name(), attribute.value()); // also writes per-key scalar
        }
        // Null-fill per-key scalars for tracked keys absent in this row
        for (const auto& [key, builder] : values_)
        {
            if (seen_attributes.find(key) == seen_attributes.end())
                appendNullAttributeScalar(key);
        }
    }

    void finish(std::vector<std::shared_ptr<arrow::Field>>& fields,
                std::vector<std::shared_ptr<arrow::Array>>& arrays,
                bool include_attributes);
};

} // namespace mldp_pvxs_driver::query::impl::mldp
