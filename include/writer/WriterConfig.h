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

#include <stdexcept>

namespace mldp_pvxs_driver::writer {

/// YAML key: top-level `writer` block.
inline constexpr char WriterKey[] = "writer";
/// YAML key: `writer.mldp` — sequence of MLDP ingestion writer instances.
inline constexpr char WriterMldpKey[] = "mldp";
/// YAML key: `writer.hdf5` — sequence of HDF5 writer instances (requires MLDP_PVXS_HDF5_ENABLED).
inline constexpr char WriterHdf5Key[] = "hdf5";

/**
 * @brief Validates the top-level `writer:` YAML block structure.
 *
 * Mirrors the reader-side pattern: no typed config objects are stored here.
 * Concrete writer configs are parsed by the factory at construction time,
 * using the raw `(type, Config)` entries from `writerEntries()`.
 *
 * `parse()` validates that each present sub-block is a sequence and that at
 * least one writer instance is configured.  Throws on any structural error.
 *
 * YAML example:
 * @code{.yaml}
 * writer:
 *   mldp:
 *     - name: mldp_main
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

    /**
     * @brief Validate the `writer:` YAML block structure.
     *
     * @param writerNode  Config node anchored at `writer`.
     * @throws WriterConfig::Error on structural errors (non-sequence sub-block,
     *         HDF5 instances without HDF5 build support).
     * @throws std::invalid_argument when no writer instance is configured.
     */
    static void validate(const config::Config& writerNode);
};

} // namespace mldp_pvxs_driver::writer
