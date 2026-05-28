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

#include <config/Config.h>
#include <pool/MLDPGrpcAnnotationPoolConfig.h>

#include <string>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Configuration for the MLDP configuration writer.
 *
 * Parsed from the YAML block that configures one configuration writer instance.
 * Handles SaveConfiguration and SaveConfigurationActivation RPCs over the
 * annotation gRPC endpoint.
 *
 * YAML mapping:
 * @code{.yaml}
 * writer:
 *   mldp-configuration:
 *     - name: cfg_writer
 *       thread-pool: 2              # optional; default: 2
 *       deadline-seconds: 10        # optional; default: 10
 *       mldp-annotation-pool:
 *         annotation-url: …
 *         min-conn: 1
 *         max-conn: 4
 * @endcode
 */
struct MLDPConfigurationWriterConfig
{
    /// Unique instance name. Required.
    std::string name;

    /// Pool configuration for the DpAnnotationService endpoint.
    util::pool::MLDPGrpcAnnotationPoolConfig poolConfig;

    /// Per-RPC deadline in seconds.  Default: 10.
    int deadlineSeconds{10};

    /// Number of worker threads draining the internal work queue.  Default: 2.
    int threadPool{2};

    /**
     * @brief Parse from a YAML config node anchored at the writer instance block.
     *
     * @throws std::runtime_error on missing required fields.
     */
    static MLDPConfigurationWriterConfig parse(const config::Config& node);
};

} // namespace mldp_pvxs_driver::writer
