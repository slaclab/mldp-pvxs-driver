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

#include <optional>
#include <stdexcept>

namespace mldp_pvxs_driver::writer {

/// YAML key for the top-level writer block.
inline constexpr char WriterKey[]       = "writer";
inline constexpr char WriterGrpcKey[]   = "grpc";
inline constexpr char WriterHdf5Key[]   = "hdf5";
inline constexpr char WriterEnabledKey[] = "enabled";

/**
 * @brief Top-level writer configuration.
 *
 * Controls which output destinations are active.  At least one writer
 * must be enabled; if none are enabled @ref parse throws
 * `std::invalid_argument`.
 *
 * Backward compatibility: configs with no `writer:` block default to
 * `grpcEnabled = true`, `hdf5Enabled = false`.
 *
 * YAML example (both writers enabled):
 * @code{.yaml}
 * writer:
 *   grpc:
 *     enabled: true
 *   hdf5:
 *     enabled: true
 *     base-path: /data/hdf5
 *     max-file-age-s: 3600
 * @endcode
 */
struct WriterConfig {
    class Error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    bool grpcEnabled{true};
    bool hdf5Enabled{false};

    std::optional<MLDPGrpcWriterConfig> grpcConfig;
    std::optional<HDF5WriterConfig>     hdf5Config;

    /** @return true if at least one writer is enabled. */
    bool anyEnabled() const noexcept { return grpcEnabled || hdf5Enabled; }

    /**
     * @brief Parse the `writer:` YAML block.
     *
     * @param writerNode  Config node anchored at `writer`.
     * @param root        Root YAML config (needed for pool and thread-pool keys).
     * @throws WriterConfig::Error on bad config.
     * @throws std::invalid_argument when no writer is enabled.
     */
    static WriterConfig parse(const config::Config& writerNode,
                              const config::Config& root);
};

} // namespace mldp_pvxs_driver::writer
