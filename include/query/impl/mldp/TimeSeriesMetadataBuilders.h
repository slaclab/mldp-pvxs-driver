//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file TimeSeriesMetadataBuilders.h
 * @brief Accumulates Arrow metadata columns for mldp.time_series record batches. */
#pragma once

#include <query/impl/mldp/MetadataArrayBuilders.h>

#include <common.pb.h>

#include <arrow/array/builder_nested.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/** @brief Builds tags, attributes, provenance, and per-key scalar Arrow arrays for time-series batches. */
class TimeSeriesMetadataBuilders : public MetadataArrayBuilders
{
public:
    TimeSeriesMetadataBuilders(const std::set<std::string>& attributes, const std::set<std::string>& provenance);

    void append(const dp::service::common::ColumnMetadata& metadata);

    void finish(std::vector<std::shared_ptr<arrow::Field>>& fields, std::vector<std::shared_ptr<arrow::Array>>& arrays);

private:
    static void appendScalars(std::map<std::string, std::unique_ptr<arrow::StringBuilder>>& builders,
                              const std::map<std::string, std::string>&                     values);

    template <typename Builder>
    static void finishCollection(const std::string& name, Builder& builder,
                                 std::vector<std::shared_ptr<arrow::Field>>& fields,
                                 std::vector<std::shared_ptr<arrow::Array>>& arrays);

    static void finishScalars(const std::string& prefix,
                              std::map<std::string, std::unique_ptr<arrow::StringBuilder>>& builders,
                              std::vector<std::shared_ptr<arrow::Field>>& fields,
                              std::vector<std::shared_ptr<arrow::Array>>& arrays);

    // Provenance builders — not in base (base handles tags + attributes only)
    std::shared_ptr<arrow::StringBuilder>                        provenance_keys_, provenance_values_;
    arrow::MapBuilder                                            provenance_;
    std::map<std::string, std::unique_ptr<arrow::StringBuilder>> provenance_values_by_key_;
};

} // namespace mldp_pvxs_driver::query::impl::mldp
