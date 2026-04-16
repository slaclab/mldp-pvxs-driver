//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/grpc/MLDPGrpcWriterConfig.h>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::config;

MLDPGrpcWriterConfig MLDPGrpcWriterConfig::parse(const Config& grpcNode)
{
    MLDPGrpcWriterConfig cfg;

    // name — required, non-empty
    cfg.name = grpcNode.get(GrpcNameKey, "");
    if (cfg.name.empty())
    {
        throw Error("writer.grpc instance is missing required '" + std::string(GrpcNameKey) + "' field");
    }

    // Pool configuration from the writer.grpc[i].mldp-pool block
    if (!grpcNode.hasChild(GrpcPoolKey))
    {
        throw Error("writer.grpc is enabled but '" + std::string(GrpcPoolKey) + "' block is missing");
    }
    const auto poolNodes = grpcNode.subConfig(GrpcPoolKey);
    if (poolNodes.empty())
    {
        throw Error("'" + std::string(GrpcPoolKey) + "' block is empty");
    }
    try
    {
        cfg.poolConfig = util::pool::MLDPGrpcPoolConfig(poolNodes.front());
    }
    catch (const util::pool::MLDPGrpcPoolConfig::Error& e)
    {
        throw Error(e.what());
    }

    // Thread pool size
    if (grpcNode.hasChild(GrpcThreadPoolKey))
    {
        const auto nodes = grpcNode.subConfig(GrpcThreadPoolKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 1;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(GrpcThreadPoolKey) + " must be > 0");
            }
            cfg.threadPoolSize = val;
        }
    }

    // Stream max bytes
    if (grpcNode.hasChild(GrpcStreamMaxBytesKey))
    {
        const auto nodes = grpcNode.subConfig(GrpcStreamMaxBytesKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 0;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(GrpcStreamMaxBytesKey) + " must be > 0");
            }
            cfg.streamMaxBytes = static_cast<std::size_t>(val);
        }
    }

    // Stream max age
    if (grpcNode.hasChild(GrpcStreamMaxAgeMsKey))
    {
        const auto nodes = grpcNode.subConfig(GrpcStreamMaxAgeMsKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 0;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(GrpcStreamMaxAgeMsKey) + " must be > 0");
            }
            cfg.streamMaxAge = std::chrono::milliseconds(val);
        }
    }

    return cfg;
}
