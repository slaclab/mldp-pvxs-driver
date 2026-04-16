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

// ---------------------------------------------------------------------------
// YAML keys owned by the gRPC writer block (under writer.grpc[i]).
// ---------------------------------------------------------------------------
inline constexpr char GrpcNameKey[]           = "name";
inline constexpr char GrpcPoolKey[]           = "mldp-pool";
inline constexpr char GrpcThreadPoolKey[]     = "thread-pool";
inline constexpr char GrpcStreamMaxBytesKey[] = "stream-max-bytes";
inline constexpr char GrpcStreamMaxAgeMsKey[] = "stream-max-age-ms";

/**
 * @brief Configuration for the gRPC ingestion writer.
 *
 * All gRPC-specific knobs live under the `writer.grpc` YAML block.
 *
 * YAML mapping:
 * @code{.yaml}
 * writer:
 *   grpc:
 *     enabled: true                   # parsed by WriterConfig, not here
 *     thread-pool: 4                  # optional; default: 1
 *     stream-max-bytes: 2097152       # optional; flush stream after this payload size
 *     stream-max-age-ms: 200          # optional; flush stream after this age in ms
 *     mldp-pool:
 *       provider-name: …
 *       ingestion-url: …
 *       query-url: …
 *       min-conn: 1
 *       max-conn: 4
 * @endcode
 */
struct MLDPGrpcWriterConfig
{
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// Underlying pool configuration (connection endpoints, credentials, …).
    util::pool::MLDPGrpcPoolConfig poolConfig;

    /// Unique instance name (required; writer.grpc[i].name).
    std::string name;

    /// Number of concurrent ingestion worker threads.
    int threadPoolSize{1};

    /// Max protobuf payload bytes per stream before flushing.
    std::size_t streamMaxBytes{2 * 1024 * 1024};

    /// Max stream age before flushing.
    std::chrono::milliseconds streamMaxAge{200};

    /**
     * @brief Parse gRPC writer configuration from the `writer.grpc` YAML node.
     *
     * @param grpcNode  Config node anchored at `writer.grpc` (contains
     *                  `mldp-pool`, `thread-pool`, `stream-max-bytes`,
     *                  `stream-max-age-ms`).
     * @throws MLDPGrpcWriterConfig::Error on validation failures.
     */
    static MLDPGrpcWriterConfig parse(const config::Config& grpcNode);
};

} // namespace mldp_pvxs_driver::writer
