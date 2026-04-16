//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/hdf5/HDF5WriterConfig.h>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::config;

HDF5WriterConfig HDF5WriterConfig::parse(const Config& node)
{
    HDF5WriterConfig cfg;

    // name — required, non-empty
    cfg.name = node.get(HDF5NameKey, "");
    if (cfg.name.empty())
    {
        throw Error("writer.hdf5 instance is missing required '" + std::string(HDF5NameKey) + "' field");
    }

    // base-path — required
    cfg.basePath = node.get(HDF5BasePathKey, "");
    if (cfg.basePath.empty())
    {
        throw Error("writer.hdf5." + std::string(HDF5BasePathKey) + " is required and must not be empty");
    }

    // max-file-age-s
    if (node.hasChild(HDF5MaxFileAgeKey))
    {
        const int val = node.getInt(HDF5MaxFileAgeKey, 0);
        if (val <= 0)
        {
            throw Error("writer.hdf5." + std::string(HDF5MaxFileAgeKey) + " must be > 0");
        }
        cfg.maxFileAge = std::chrono::seconds(val);
    }

    // max-file-size-mb
    if (node.hasChild(HDF5MaxFileSizeMBKey))
    {
        const int val = node.getInt(HDF5MaxFileSizeMBKey, 0);
        if (val <= 0)
        {
            throw Error("writer.hdf5." + std::string(HDF5MaxFileSizeMBKey) + " must be > 0");
        }
        cfg.maxFileSizeMB = static_cast<uint64_t>(val);
    }

    // flush-interval-ms
    if (node.hasChild(HDF5FlushIntervalMsKey))
    {
        const int val = node.getInt(HDF5FlushIntervalMsKey, 0);
        if (val <= 0)
        {
            throw Error("writer.hdf5." + std::string(HDF5FlushIntervalMsKey) + " must be > 0");
        }
        cfg.flushInterval = std::chrono::milliseconds(val);
    }

    // compression-level
    if (node.hasChild(HDF5CompressionKey))
    {
        const int val = node.getInt(HDF5CompressionKey, 0);
        if (val < 0 || val > 9)
        {
            throw Error("writer.hdf5." + std::string(HDF5CompressionKey) + " must be in [0, 9]");
        }
        cfg.compressionLevel = val;
    }

    return cfg;
}
