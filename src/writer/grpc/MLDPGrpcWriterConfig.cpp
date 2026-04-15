//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <controller/MLDPPVXSControllerConfig.h>
#include <writer/grpc/MLDPGrpcWriterConfig.h>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::config;

MLDPGrpcWriterConfig MLDPGrpcWriterConfig::parse(const Config& root)
{
    MLDPGrpcWriterConfig cfg;

    // Pool configuration from root mldp-pool block
    if (!root.hasChild(MldpPoolKey))
    {
        throw Error("writer.grpc is enabled but '" + std::string(MldpPoolKey) + "' block is missing");
    }
    const auto poolNodes = root.subConfig(MldpPoolKey);
    if (poolNodes.empty())
    {
        throw Error("'" + std::string(MldpPoolKey) + "' block is empty");
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
    if (root.hasChild(ControllerThreadPoolKey))
    {
        const auto nodes = root.subConfig(ControllerThreadPoolKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 1;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(ControllerThreadPoolKey) + " must be > 0");
            }
            cfg.threadPoolSize = val;
        }
    }

    // Stream max bytes
    if (root.hasChild(ControllerStreamMaxBytesKey))
    {
        const auto nodes = root.subConfig(ControllerStreamMaxBytesKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 0;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(ControllerStreamMaxBytesKey) + " must be > 0");
            }
            cfg.streamMaxBytes = static_cast<std::size_t>(val);
        }
    }

    // Stream max age
    if (root.hasChild(ControllerStreamMaxAgeMsKey))
    {
        const auto nodes = root.subConfig(ControllerStreamMaxAgeMsKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 0;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(ControllerStreamMaxAgeMsKey) + " must be > 0");
            }
            cfg.streamMaxAge = std::chrono::milliseconds(val);
        }
    }

    return cfg;
}
