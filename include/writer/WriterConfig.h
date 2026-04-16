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
#include <writer/grpc/MLDPGrpcWriterConfig.h>
#include <writer/hdf5/HDF5WriterConfig.h>

#include <stdexcept>
#include <vector>

namespace mldp_pvxs_driver::writer {

/// YAML key: top-level `writer` block.
inline constexpr char WriterKey[] = "writer";
/// YAML key: `writer.grpc` — sequence of gRPC writer instances.
inline constexpr char WriterGrpcKey[] = "grpc";
/// YAML key: `writer.hdf5` — sequence of HDF5 writer instances (requires MLDP_PVXS_HDF5_ENABLED).
inline constexpr char WriterHdf5Key[] = "hdf5";

/**
 * @brief Top-level writer configuration.
 *
 * Each writer type is configured as a YAML sequence; each sequence entry
 * becomes one writer instance identified by its `name` field.  Presence in
 * the sequence is sufficient — there is no separate `enabled` flag.  At
 * least one instance (of any type) must be configured.
 *
 * YAML example (two gRPC writers + one HDF5 writer):
 * @code{.yaml}
 * writer:
 *   grpc:
 *     - name: grpc_main
 *       thread-pool: 2
 *       mldp-pool: { ... }
 *     - name: grpc_backup
 *       mldp-pool: { ... }
 *   hdf5:
 *     - name: hdf5_local
 *       base-path: /data/hdf5
 * @endcode
 */
struct WriterConfig
{
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// YAML key: `writer.grpc` — one entry per configured gRPC writer instance (preserves declaration order).
    std::vector<MLDPGrpcWriterConfig> grpcConfigs;

    /// YAML key: `writer.hdf5` — one entry per configured HDF5 writer instance (preserves declaration order).
    std::vector<HDF5WriterConfig> hdf5Configs;

    /** @return true if at least one writer instance is configured. */
    bool anyEnabled() const noexcept
    {
        return !grpcConfigs.empty() || !hdf5Configs.empty();
    }

    /**
     * @brief Parse the `writer:` YAML block.
     *
     * @param writerNode  Config node anchored at `writer`.
     * @throws WriterConfig::Error on bad config (bad sequence shape, parse errors).
     * @throws std::invalid_argument when no writer instance is configured.
     */
    static WriterConfig parse(const config::Config& writerNode);
};

} // namespace mldp_pvxs_driver::writer
