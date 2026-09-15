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

#include <pool/MLDPGrpcPoolConfig.h>

namespace mldp_pvxs_driver::util::pool {

/**
 * @brief Query-only pool configuration.
 *
 * Subclass of MLDPGrpcPoolConfig that requires only:
 *   query-url, min-conn, max-conn (and optional credentials).
 *
 * ingestion-url and provider-name are irrelevant for the query path
 * and are left empty.  Use this config when building a queryable:
 * block where no ingestion writer is present.
 *
 * YAML example:
 * @code{.yaml}
 * mldp-pool:
 *   query-url: grpc://dp-query:50052
 *   min-conn: 1
 *   max-conn: 2
 * @endcode
 */
class MLDPGrpcQueryPoolConfig : public MLDPGrpcPoolConfig
{
public:
    MLDPGrpcQueryPoolConfig() = default;

    /** Parse from a config node containing only query-pool fields. */
    explicit MLDPGrpcQueryPoolConfig(const config::Config& node);

    /** Construct by copying query-relevant fields from a full MLDPGrpcPoolConfig. */
    explicit MLDPGrpcQueryPoolConfig(const MLDPGrpcPoolConfig& full);
};

} // namespace mldp_pvxs_driver::util::pool
