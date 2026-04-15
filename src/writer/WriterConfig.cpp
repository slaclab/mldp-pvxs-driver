//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/WriterConfig.h>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::config;

WriterConfig WriterConfig::parse(const Config& writerNode, const Config& root)
{
    WriterConfig cfg;
    cfg.grpcEnabled = true;   // default
    cfg.hdf5Enabled = false;  // default

    // -- gRPC writer --
    if (writerNode.hasChild(WriterGrpcKey))
    {
        const auto grpcNodes = writerNode.subConfig(WriterGrpcKey);
        if (!grpcNodes.empty())
        {
            const auto& grpcNode = grpcNodes.front();
            cfg.grpcEnabled = grpcNode.getBool(WriterEnabledKey, true);
            if (cfg.grpcEnabled)
            {
                try
                {
                    cfg.grpcConfig = MLDPGrpcWriterConfig::parse(root);
                }
                catch (const MLDPGrpcWriterConfig::Error& e)
                {
                    throw Error(std::string("writer.grpc: ") + e.what());
                }
            }
        }
    }
    else
    {
        // No writer.grpc block → default to enabled; parse pool from root
        try
        {
            cfg.grpcConfig = MLDPGrpcWriterConfig::parse(root);
        }
        catch (const MLDPGrpcWriterConfig::Error& e)
        {
            throw Error(std::string("writer.grpc: ") + e.what());
        }
    }

    // -- HDF5 writer --
    if (writerNode.hasChild(WriterHdf5Key))
    {
        const auto hdf5Nodes = writerNode.subConfig(WriterHdf5Key);
        if (!hdf5Nodes.empty())
        {
            const auto& hdf5Node = hdf5Nodes.front();
            cfg.hdf5Enabled = hdf5Node.getBool(WriterEnabledKey, false);
            if (cfg.hdf5Enabled)
            {
#ifndef MLDP_PVXS_HDF5_ENABLED
                throw Error("writer.hdf5.enabled=true but this build was compiled without HDF5 support "
                            "(MLDP_PVXS_ENABLE_HDF5=OFF)");
#else
                try
                {
                    cfg.hdf5Config = HDF5WriterConfig::parse(hdf5Node);
                }
                catch (const HDF5WriterConfig::Error& e)
                {
                    throw Error(std::string("writer.hdf5: ") + e.what());
                }
#endif
            }
        }
    }

    if (!cfg.anyEnabled())
    {
        throw std::invalid_argument("writer config: at least one writer must be enabled");
    }

    return cfg;
}
