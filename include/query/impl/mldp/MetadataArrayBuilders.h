//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file MetadataArrayBuilders.h
 * @brief Base class for shared tags / attributes / per-key scalar Arrow builder logic. */
#pragma once

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_nested.h>
#include <arrow/record_batch.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/**
 * @brief Owns and drives the shared Arrow builders for tags (list), attributes (map),
 *        and per-key scalar string columns common to all MLDP metadata record types.
 *
 * Subclasses call the protected helpers from their own @c append() and @c finish() methods.
 */
class MetadataArrayBuilders
{
public:
    explicit MetadataArrayBuilders(const std::set<std::string>& attribute_keys);

    // Non-copyable, non-movable — holds Arrow builder references
    MetadataArrayBuilders(const MetadataArrayBuilders&) = delete;
    MetadataArrayBuilders& operator=(const MetadataArrayBuilders&) = delete;

protected:
    /** Call at the start of each logical row to open the tags list and attributes map. */
    void beginTagsAndAttributes();

    /** Append a single tag string into the open tags list entry. */
    void appendTag(std::string_view tag);

    /** Append one key/value pair into the open attributes map entry and track per-key scalar. */
    void appendAttributeEntry(std::string_view key, std::string_view value);

    /** Append null for a per-key scalar whose key was absent in this row. */
    void appendNullAttributeScalar(const std::string& key);

    /** Finish tags + attributes arrays and push them into fields/arrays. */
    void finishTagsAndAttributes(std::vector<std::shared_ptr<arrow::Field>>& fields,
                                 std::vector<std::shared_ptr<arrow::Array>>& arrays,
                                 bool include_attributes_map);

    /** Finish per-key scalar string columns, prefixing each field name with @p prefix. */
    void finishAttributeScalars(const std::string& prefix,
                                std::vector<std::shared_ptr<arrow::Field>>& fields,
                                std::vector<std::shared_ptr<arrow::Array>>& arrays);

    std::shared_ptr<arrow::StringBuilder>                        tags_values_;
    arrow::ListBuilder                                           tags_;
    std::shared_ptr<arrow::StringBuilder>                        attributes_keys_;
    std::shared_ptr<arrow::StringBuilder>                        attributes_values_;
    arrow::MapBuilder                                            attributes_;
    std::map<std::string, std::unique_ptr<arrow::StringBuilder>> values_;
};

} // namespace mldp_pvxs_driver::query::impl::mldp
