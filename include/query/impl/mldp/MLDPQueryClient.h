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
#include <metrics/Metrics.h>
#include <pool/MLDPGrpcPoolConfig.h>
#include <pool/MLDPGrpcQueryPool.h>
#include <util/bus/IDataBus.h>
#include <util/log/Logger.h>
#include <query/IQueryable.h>

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::query::impl::mldp {

/**
 * @brief Standalone MLDP query client.
 *
 * Encapsulates the `queryPvMetadata` / `queryData` RPCs that were previously
 * embedded in `MLDPPVXSController`.  The controller is now concerned only with
 * reading from EPICS and writing to writers; all query/inspection duties live
 * here.
 *
 * Typical use — integration tests and diagnostic tools:
 * @code
 * MLDPQueryClient client(pool_config);
 * auto infos = client.querySourcesInfo({"MY:PV"});
 * auto data  = client.querySourcesData({"MY:PV"}, options);
 * @endcode
 */
class MLDPQueryClient : public IQueryable
{
public:
    /**
     * @brief Construct and immediately initialise the underlying query pool.
     *
     * @param poolConfig  Connection parameters (endpoints, provider, pool size).
     * @param metrics     Optional shared metrics collector.
     */
    explicit MLDPQueryClient(const util::pool::MLDPGrpcPoolConfig& poolConfig,
                             std::shared_ptr<metrics::Metrics>     metrics = nullptr);

    ~MLDPQueryClient() override = default;

    // Non-copyable, movable
    MLDPQueryClient(const MLDPQueryClient&) = delete;
    MLDPQueryClient& operator=(const MLDPQueryClient&) = delete;
    MLDPQueryClient(MLDPQueryClient&&) = default;
    MLDPQueryClient& operator=(MLDPQueryClient&&) = default;

    /**
     * @brief Query MLDP metadata for a set of source identifiers.
     *
     * Attempts `queryPvMetadata` RPC first (preferred).  Falls back to
     * `queryData` + timestamp-range extraction when the RPC is absent on
     * older deployments.
     *
     * @param source_names Source/PV identifiers to query.
     * @return Metadata rows for the sources known to the backend.
     */
    std::vector<util::bus::IDataBus::SourceInfo>
    querySourcesInfo(const std::set<std::string>& source_names) override;

    /**
     * @brief Query MLDP data values for sources over a relative time window.
     *
     * Retries until @p options.timeout expires because source visibility is
     * eventually consistent on some deployments.
     *
     * @param source_names Source/PV identifiers to query.
     * @param options      Query tuning options (timeouts and relative window).
     * @return Map keyed by source name with one DataValues entry per bucket,
     *         or std::nullopt on transport/protocol failure.
     */
    std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>>
    querySourcesData(const std::set<std::string>&              source_names,
                     const util::bus::QuerySourcesDataOptions& options = {}) override;

private:
    std::shared_ptr<util::log::ILogger>                     logger_;
    util::pool::MLDPGrpcQueryPool::MLDPGrpcQueryPoolShrdPtr pool_;
};

} // namespace mldp_pvxs_driver::query::impl::mldp
