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

// YAML keys owned by the MLDP writer block (under writer.mldp[i]).
/// YAML key: `writer.mldp[i].name` — required, unique instance name.
inline constexpr char MldpNameKey[] = "name";
/// YAML key: `writer.mldp[i].mldp-pool` — required, connection pool sub-block.
inline constexpr char MldpPoolKey[] = "mldp-pool";
/// YAML key: `writer.mldp[i].thread-pool` — worker thread count (default: 1).
inline constexpr char MldpThreadPoolKey[] = "thread-pool";
/// YAML key: `writer.mldp[i].stream-max-bytes` — flush stream after this payload size in bytes (default: 2097152).
inline constexpr char MldpStreamMaxBytesKey[] = "stream-max-bytes";
/// YAML key: `writer.mldp[i].stream-max-age-ms` — flush stream after this age in ms (default: 200).
inline constexpr char MldpStreamMaxAgeMsKey[] = "stream-max-age-ms";

/**
 * @brief Configuration for the MLDP ingestion writer.
 *
 * All MLDP-specific knobs live under the `writer.mldp` YAML block.
 *
 * YAML mapping:
 * @code{.yaml}
 * writer:
 *   mldp:
 *     - name: mldp_main
 *       thread-pool: 4                  # optional; default: 1
 *       stream-max-bytes: 2097152       # optional; flush stream after this payload size
 *       stream-max-age-ms: 200          # optional; flush stream after this age in ms
 *       mldp-pool:
 *         provider-name: …
 *         ingestion-url: …
 *         query-url: …
 *         min-conn: 1
 *         max-conn: 4
 * @endcode
 */
struct MLDPWriterConfig
{
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// YAML key: `writer.mldp[i].mldp-pool` — underlying pool configuration (connection endpoints, credentials).
    util::pool::MLDPGrpcPoolConfig poolConfig;

    /// YAML key: `writer.mldp[i].name` — unique instance name.  Required.
    std::string name;

    /// YAML key: `writer.mldp[i].thread-pool` — number of concurrent ingestion worker threads.  Default: 1.
    int threadPoolSize{1};

    /// YAML key: `writer.mldp[i].stream-max-bytes` — max protobuf payload bytes per stream before flushing.  Default: 2 MiB.
    std::size_t streamMaxBytes{2 * 1024 * 1024};

    /// YAML key: `writer.mldp[i].stream-max-age-ms` — max stream age before flushing.  Default: 200 ms.
    std::chrono::milliseconds streamMaxAge{200};

    /**
     * @brief Parse MLDP writer configuration from the `writer.mldp` YAML node.
     *
     * @param mldpNode  Config node anchored at `writer.mldp` (contains
     *                  `mldp-pool`, `thread-pool`, `stream-max-bytes`,
     *                  `stream-max-age-ms`).
     * @throws MLDPWriterConfig::Error on validation failures.
     */
    static MLDPWriterConfig parse(const config::Config& mldpNode);
};

} // namespace mldp_pvxs_driver::writer
