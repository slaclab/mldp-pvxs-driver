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

WriterConfig WriterConfig::parse(const Config& writerNode)
{
    WriterConfig cfg;

    // -- gRPC writer instances --
    if (writerNode.hasChild(WriterGrpcKey))
    {
        if (!writerNode.isSequence(WriterGrpcKey))
        {
            throw Error("writer.grpc must be a sequence of writer instances");
        }
        const auto grpcItems = writerNode.subConfig(WriterGrpcKey);
        for (const auto& item : grpcItems)
        {
            try
            {
                cfg.grpcConfigs.push_back(MLDPGrpcWriterConfig::parse(item));
            }
            catch (const MLDPGrpcWriterConfig::Error& e)
            {
                throw Error(std::string("writer.grpc: ") + e.what());
            }
        }
    }

    // -- HDF5 writer instances --
    if (writerNode.hasChild(WriterHdf5Key))
    {
        if (!writerNode.isSequence(WriterHdf5Key))
        {
            throw Error("writer.hdf5 must be a sequence of writer instances");
        }
#ifndef MLDP_PVXS_HDF5_ENABLED
        throw Error("writer.hdf5 instances are configured but this build was compiled without "
                    "HDF5 support (MLDP_PVXS_ENABLE_HDF5=OFF)");
#else
        const auto hdf5Items = writerNode.subConfig(WriterHdf5Key);
        for (const auto& item : hdf5Items)
        {
            try
            {
                cfg.hdf5Configs.push_back(HDF5WriterConfig::parse(item));
            }
            catch (const HDF5WriterConfig::Error& e)
            {
                throw Error(std::string("writer.hdf5: ") + e.what());
            }
        }
#endif
    }

    if (!cfg.anyEnabled())
    {
        throw std::invalid_argument("writer config: at least one writer instance must be configured");
    }

    return cfg;
}
