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

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::writer {

// YAML keys for the writer.hdf5[i] block.
/// YAML key: `writer.hdf5[i].name` — required, unique instance name.
inline constexpr char HDF5NameKey[] = "name";
/// YAML key: `writer.hdf5[i].base-path` — required, output directory for HDF5 files.
inline constexpr char HDF5BasePathKey[] = "base-path";
/// YAML key: `writer.hdf5[i].max-file-age-s` — rotate file after N seconds (default: 3600).
inline constexpr char HDF5MaxFileAgeKey[] = "max-file-age-s";
/// YAML key: `writer.hdf5[i].max-file-size-mb` — rotate file at N MiB (default: 512).
inline constexpr char HDF5MaxFileSizeMBKey[] = "max-file-size-mb";
/// YAML key: `writer.hdf5[i].flush-interval-ms` — flush thread period in ms (default: 1000).
inline constexpr char HDF5FlushIntervalMsKey[] = "flush-interval-ms";
/// YAML key: `writer.hdf5[i].compression-level` — DEFLATE level 0–9 (default: 0 = off).
inline constexpr char HDF5CompressionKey[] = "compression-level";

/**
 * @brief Configuration for the HDF5 file writer.
 *
 * Maps the `writer.hdf5` YAML block.  One file per root_source is
 * created under @ref basePath; files are rotated when either the age
 * or the size threshold is exceeded.
 */
struct HDF5WriterConfig
{
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// YAML key: `writer.hdf5[i].base-path` — directory where HDF5 files are created.  Required.
    std::string basePath;

    /// YAML key: `writer.hdf5[i].name` — unique instance name.  Required.
    std::string name;

    /// YAML key: `writer.hdf5[i].max-file-age-s` — maximum age of an open file before rotation.  Default: 3600 s (1 hour).
    std::chrono::seconds maxFileAge{3600};

    /// YAML key: `writer.hdf5[i].max-file-size-mb` — maximum size (MiB) of an open file before rotation.  Default: 512 MiB.
    uint64_t maxFileSizeMB{512};

    /// YAML key: `writer.hdf5[i].flush-interval-ms` — how often the flush thread calls H5File::flush.  Default: 1000 ms.
    std::chrono::milliseconds flushInterval{1000};

    /// YAML key: `writer.hdf5[i].compression-level` — DEFLATE level 0–9 (0 = no compression).  Default: 0.
    int compressionLevel{0};

    /**
     * @brief Parse the writer.hdf5 YAML sub-node.
     *
     * @param node  Config node anchored at `writer.hdf5`.
     * @throws HDF5WriterConfig::Error on validation failures.
     */
    static HDF5WriterConfig parse(const config::Config& node);
};

} // namespace mldp_pvxs_driver::writer
