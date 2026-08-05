//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/MetadataArrayBuilders.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query::impl::mldp;

MetadataArrayBuilders::MetadataArrayBuilders(const std::set<std::string>& attribute_keys)
    : tags_values_(std::make_shared<arrow::StringBuilder>())
    , tags_(arrow::default_memory_pool(), tags_values_)
    , attributes_keys_(std::make_shared<arrow::StringBuilder>())
    , attributes_values_(std::make_shared<arrow::StringBuilder>())
    , attributes_(arrow::default_memory_pool(), attributes_keys_, attributes_values_)
{
    for (const auto& key : attribute_keys)
        values_.emplace(key, std::make_unique<arrow::StringBuilder>());
}

void MetadataArrayBuilders::beginTagsAndAttributes()
{
    if (!tags_.Append().ok() || !attributes_.Append().ok())
        throw std::runtime_error("Failed to begin Arrow metadata collection");
}

void MetadataArrayBuilders::appendTag(std::string_view tag)
{
    if (!tags_values_->Append(tag).ok())
        throw std::runtime_error("Failed to append Arrow metadata tag");
}

void MetadataArrayBuilders::appendAttributeEntry(std::string_view key, std::string_view value)
{
    if (!attributes_keys_->Append(key).ok() || !attributes_values_->Append(value).ok())
        throw std::runtime_error("Failed to append Arrow metadata attribute");
    // Also populate the per-key scalar builder if this key is tracked
    const auto it = values_.find(std::string(key));
    if (it != values_.end())
        if (!it->second->Append(value).ok())
            throw std::runtime_error("Failed to append Arrow metadata attribute scalar");
}

void MetadataArrayBuilders::appendNullAttributeScalar(const std::string& key)
{
    const auto it = values_.find(key);
    if (it != values_.end())
        if (!it->second->AppendNull().ok())
            throw std::runtime_error("Failed to append null dynamic metadata value");
}

void MetadataArrayBuilders::finishTagsAndAttributes(std::vector<std::shared_ptr<arrow::Field>>& fields,
                                                    std::vector<std::shared_ptr<arrow::Array>>& arrays,
                                                    const bool include_attributes_map)
{
    std::shared_ptr<arrow::Array> tags;
    std::shared_ptr<arrow::Array> attributes;
    if (!tags_.Finish(&tags).ok() || !attributes_.Finish(&attributes).ok())
        throw std::runtime_error("Failed to finish Arrow metadata collections");
    fields.push_back(arrow::field("tags", tags->type()));
    arrays.push_back(std::move(tags));
    if (include_attributes_map)
    {
        fields.push_back(arrow::field("attributes", attributes->type()));
        arrays.push_back(std::move(attributes));
    }
}

void MetadataArrayBuilders::finishAttributeScalars(const std::string& prefix,
                                                   std::vector<std::shared_ptr<arrow::Field>>& fields,
                                                   std::vector<std::shared_ptr<arrow::Array>>& arrays)
{
    for (auto& [key, builder] : values_)
    {
        std::shared_ptr<arrow::Array> value;
        if (!builder->Finish(&value).ok())
            throw std::runtime_error("Failed to finish dynamic metadata value");
        fields.push_back(arrow::field(prefix + key, arrow::utf8(), true));
        arrays.push_back(std::move(value));
    }
}
