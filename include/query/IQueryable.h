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

#include <common.pb.h>
#include <util/bus/IDataBus.h>

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::query {

/**
 * @brief Pure interface for MLDP query operations.
 *
 * Allows alternative backends (mock, REST, archiver-based) to be injected
 * without touching consumers.  `MLDPQueryClient` is the canonical production
 * implementation.
 */
class IQueryable
{
public:
    virtual ~IQueryable() = default;

    /**
     * @brief Query metadata for a set of source identifiers.
     */
    virtual std::vector<util::bus::IDataBus::SourceInfo>
    querySourcesInfo(const std::set<std::string>& source_names) = 0;

    /**
     * @brief Query data values for sources over a relative time window.
     */
    virtual std::optional<
        std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>>
    querySourcesData(const std::set<std::string>&              source_names,
                     const util::bus::QuerySourcesDataOptions& options = {}) = 0;
};

} // namespace mldp_pvxs_driver::query
