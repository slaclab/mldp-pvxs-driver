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
 * @brief Annotation-only pool configuration.
 *
 * Subclass of MLDPGrpcPoolConfig that requires only:
 *   annotation-url, min-conn, max-conn (and optional credentials).
 *
 * ingestion-url, query-url, provider-name are irrelevant for writers
 * that call only DpAnnotationService RPCs — they are left empty.
 *
 * Shares Credentials enum, Type values, and key constants from base.
 *
 * YAML example:
 * @code{.yaml}
 * my-annotation-pool:
 *   annotation-url: dp-annotation:50053
 *   min-conn: 1
 *   max-conn: 4
 * @endcode
 */
class MLDPGrpcAnnotationPoolConfig : public MLDPGrpcPoolConfig
{
public:
    MLDPGrpcAnnotationPoolConfig() = default;

    /** Parse from a config node containing only annotation-pool fields. */
    explicit MLDPGrpcAnnotationPoolConfig(const config::Config& node);

    /** Construct by copying annotation-relevant fields from a full MLDPGrpcPoolConfig. */
    explicit MLDPGrpcAnnotationPoolConfig(const MLDPGrpcPoolConfig& full);
};

} // namespace mldp_pvxs_driver::util::pool
