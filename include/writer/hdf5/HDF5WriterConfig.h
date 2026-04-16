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

/// YAML keys for the writer.hdf5[i] block.
inline constexpr char HDF5NameKey[] = "name";
inline constexpr char HDF5BasePathKey[] = "base-path";
inline constexpr char HDF5MaxFileAgeKey[] = "max-file-age-s";
inline constexpr char HDF5MaxFileSizeMBKey[] = "max-file-size-mb";
inline constexpr char HDF5FlushIntervalMsKey[] = "flush-interval-ms";
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

    /// Directory where HDF5 files are created.  Required.
    std::string basePath;

    /// Unique instance name (required; writer.hdf5[i].name).
    std::string name;

    /// Maximum age of an open file before rotation.  Default: 1 hour.
    std::chrono::seconds maxFileAge{3600};

    /// Maximum size (MiB) of an open file before rotation.  Default: 512 MiB.
    uint64_t maxFileSizeMB{512};

    /// How often the flush thread calls H5File::flush.  Default: 1 s.
    std::chrono::milliseconds flushInterval{1000};

    /// DEFLATE compression level 0–9 (0 = no compression).  Default: 0.
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
