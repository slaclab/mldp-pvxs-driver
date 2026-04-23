//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/mldp/MLDPWriterConfig.h>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::config;

MLDPWriterConfig MLDPWriterConfig::parse(const Config& mldpNode)
{
    MLDPWriterConfig cfg;

    // name — required, non-empty
    cfg.name = mldpNode.get(MldpNameKey, "");
    if (cfg.name.empty())
    {
        throw Error("writer.mldp instance is missing required '" + std::string(MldpNameKey) + "' field");
    }

    // Pool configuration from the writer.mldp[i].mldp-pool block
    if (!mldpNode.hasChild(MldpPoolKey))
    {
        throw Error("writer.mldp is enabled but '" + std::string(MldpPoolKey) + "' block is missing");
    }
    const auto poolNodes = mldpNode.subConfig(MldpPoolKey);
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
    if (mldpNode.hasChild(MldpThreadPoolKey))
    {
        const auto nodes = mldpNode.subConfig(MldpThreadPoolKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 1;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(MldpThreadPoolKey) + " must be > 0");
            }
            cfg.threadPoolSize = val;
        }
    }

    // Stream max bytes
    if (mldpNode.hasChild(MldpStreamMaxBytesKey))
    {
        const auto nodes = mldpNode.subConfig(MldpStreamMaxBytesKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 0;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(MldpStreamMaxBytesKey) + " must be > 0");
            }
            cfg.streamMaxBytes = static_cast<std::size_t>(val);
        }
    }

    // Stream max age
    if (mldpNode.hasChild(MldpStreamMaxAgeMsKey))
    {
        const auto nodes = mldpNode.subConfig(MldpStreamMaxAgeMsKey);
        if (!nodes.empty() && nodes.front().raw().has_val())
        {
            int val = 0;
            nodes.front() >> val;
            if (val <= 0)
            {
                throw Error(std::string(MldpStreamMaxAgeMsKey) + " must be > 0");
            }
            cfg.streamMaxAge = std::chrono::milliseconds(val);
        }
    }

    return cfg;
}
