//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/impl/mldp/TimeSeriesMetadataBuilders.h>

#include <arrow/record_batch.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query::impl::mldp;

TimeSeriesMetadataBuilders::TimeSeriesMetadataBuilders(const std::set<std::string>& attributes,
                                                       const std::set<std::string>& provenance)
    : MetadataArrayBuilders(attributes)
    , provenance_keys_(std::make_shared<arrow::StringBuilder>())
    , provenance_values_(std::make_shared<arrow::StringBuilder>())
    , provenance_(arrow::default_memory_pool(), provenance_keys_, provenance_values_)
{
    for (const auto& key : provenance)
        provenance_values_by_key_.emplace(key, std::make_unique<arrow::StringBuilder>());
}

void TimeSeriesMetadataBuilders::append(const dp::service::common::ColumnMetadata& metadata)
{
    if (!provenance_.Append().ok())
        throw std::runtime_error("Failed to begin Arrow time-series provenance collection");
    beginTagsAndAttributes();
    for (const auto& tag : metadata.tags())
        appendTag(tag);
    std::map<std::string, std::string> attributes;
    for (const auto& attribute : metadata.attributes())
    {
        attributes[attribute.name()] = attribute.value();
        appendAttributeEntry(attribute.name(), attribute.value());
    }
    // Finish per-key attribute scalars via base helper by calling appendNullAttributeScalar
    // for keys absent in this row (values_ is the base attribute scalar map)
    for (auto& [key, builder] : values_)
    {
        if (attributes.find(key) == attributes.end())
            appendNullAttributeScalar(key);
    }
    std::map<std::string, std::string> provenance;
    if (!metadata.provenance().source().empty())
        provenance.emplace("source", metadata.provenance().source());
    if (!metadata.provenance().process().empty())
        provenance.emplace("process", metadata.provenance().process());
    for (const auto& [key, value] : provenance)
    {
        if (!provenance_keys_->Append(key).ok() || !provenance_values_->Append(value).ok())
            throw std::runtime_error("Failed to append Arrow time-series provenance entry");
    }
    appendScalars(provenance_values_by_key_, provenance);
}

void TimeSeriesMetadataBuilders::finish(std::vector<std::shared_ptr<arrow::Field>>& fields,
                                        std::vector<std::shared_ptr<arrow::Array>>& arrays)
{
    finishTagsAndAttributes(fields, arrays, true);
    finishCollection("provenance", provenance_, fields, arrays);
    finishAttributeScalars("attributes.", fields, arrays);
    finishScalars("provenance.", provenance_values_by_key_, fields, arrays);
}

void TimeSeriesMetadataBuilders::appendScalars(std::map<std::string, std::unique_ptr<arrow::StringBuilder>>& builders,
                                               const std::map<std::string, std::string>&                     values)
{
    for (auto& [key, builder] : builders)
        if (const auto it = values.find(key); it != values.end())
        {
            if (!builder->Append(it->second).ok())
                throw std::runtime_error("Failed to append Arrow metadata scalar");
        }
        else if (!builder->AppendNull().ok())
        {
            throw std::runtime_error("Failed to append null metadata scalar");
        }
}

template <typename Builder>
void TimeSeriesMetadataBuilders::finishCollection(const std::string& name, Builder& builder,
                                                   std::vector<std::shared_ptr<arrow::Field>>& fields,
                                                   std::vector<std::shared_ptr<arrow::Array>>& arrays)
{
    std::shared_ptr<arrow::Array> array;
    if (!builder.Finish(&array).ok())
        throw std::runtime_error("Failed to finish Arrow metadata collection");
    fields.push_back(arrow::field(name, array->type()));
    arrays.push_back(std::move(array));
}

void TimeSeriesMetadataBuilders::finishScalars(const std::string& prefix,
                                               std::map<std::string, std::unique_ptr<arrow::StringBuilder>>& builders,
                                               std::vector<std::shared_ptr<arrow::Field>>& fields,
                                               std::vector<std::shared_ptr<arrow::Array>>& arrays)
{
    for (auto& [key, builder] : builders)
    {
        std::shared_ptr<arrow::Array> array;
        if (!builder->Finish(&array).ok())
            throw std::runtime_error("Failed to finish Arrow metadata scalar");
        fields.push_back(arrow::field(prefix + key, array->type(), true));
        arrays.push_back(std::move(array));
    }
}

// Explicit instantiations for finishCollection<>
template void TimeSeriesMetadataBuilders::finishCollection<arrow::MapBuilder>(
    const std::string&, arrow::MapBuilder&,
    std::vector<std::shared_ptr<arrow::Field>>&, std::vector<std::shared_ptr<arrow::Array>>&);
