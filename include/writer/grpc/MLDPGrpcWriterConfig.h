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
#include <pool/MLDPGrpcPoolConfig.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Configuration for the gRPC ingestion writer.
 *
 * Extracts the gRPC-specific knobs that were previously embedded in
 * `MLDPPVXSControllerConfig` and collocates them with the writer they
 * govern.
 *
 * YAML mapping:
 * @code{.yaml}
 * writer:
 *   grpc:
 *     enabled: true    # parsed by WriterConfig, not here
 * # pool config is read from the root mldp-pool block
 * mldp-pool:
 *   provider-name: …
 *   …
 * controller-thread-pool: 4
 * controller-stream-max-bytes: 2097152
 * controller-stream-max-age-ms: 200
 * @endcode
 */
struct MLDPGrpcWriterConfig {
    class Error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /// Underlying pool configuration (connection endpoints, credentials, …).
    util::pool::MLDPGrpcPoolConfig poolConfig;

    /// Number of concurrent ingestion worker threads.
    int threadPoolSize{1};

    /// Max protobuf payload bytes per stream before flushing.
    std::size_t streamMaxBytes{2 * 1024 * 1024};

    /// Max stream age before flushing.
    std::chrono::milliseconds streamMaxAge{200};

    /**
     * @brief Parse gRPC writer configuration.
     *
     * @param root  Root YAML config node (contains `mldp-pool`,
     *              `controller-thread-pool`, `controller-stream-*`).
     * @throws MLDPGrpcWriterConfig::Error on validation failures.
     */
    static MLDPGrpcWriterConfig parse(const config::Config& root);
};

} // namespace mldp_pvxs_driver::writer
