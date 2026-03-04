//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "config/Config.h"
#include <controller/MLDPPVXSControllerConfig.h>


using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::config;

namespace {
std::string pickKey(const Config& cfg, const std::string& dashKey, const std::string& underscoreKey)
{
    if (cfg.hasChild(dashKey))
    {
        return dashKey;
    }
    return underscoreKey;
}
} // namespace

MLDPPVXSControllerConfig::MLDPPVXSControllerConfig() = default;

MLDPPVXSControllerConfig::MLDPPVXSControllerConfig(const ::mldp_pvxs_driver::config::Config& root)
{
    if (!root.valid())
    {
        throw Error("Controller configuration node is invalid");
    }

    parse(root);
}

bool MLDPPVXSControllerConfig::valid() const
{
    return valid_;
}

const MLDPGrpcPoolConfig& MLDPPVXSControllerConfig::pool() const
{
    return pool_;
}

const std::string& MLDPPVXSControllerConfig::providerName() const
{
    return pool_.providerName();
}

int MLDPPVXSControllerConfig::controllerThreadPoolSize() const
{
    return controllerThreadPoolSize_;
}

std::size_t MLDPPVXSControllerConfig::controllerStreamMaxBytes() const
{
    return controllerStreamMaxBytes_;
}


std::chrono::milliseconds MLDPPVXSControllerConfig::controllerStreamMaxAge() const
{
    return controllerStreamMaxAge_;
}

const std::vector<Config>&
MLDPPVXSControllerConfig::readerConfigs() const
{
    return readerConfigs_;
}

const std::vector<std::pair<std::string, Config>>&
MLDPPVXSControllerConfig::readerEntries() const
{
    return readerEntries_;
}

const std::optional<MetricsConfig>& MLDPPVXSControllerConfig::metricsConfig() const
{
    return metricsConfig_;
}

void MLDPPVXSControllerConfig::parse(const ::mldp_pvxs_driver::config::Config& root)
{
    parseThreadPool(root);
    parsePool(root);
    parseStreamLimits(root);
    parseReaders(root);
    parseMetrics(root);
    valid_ = true;
}

void MLDPPVXSControllerConfig::parseThreadPool(const ::mldp_pvxs_driver::config::Config& root)
{
    const auto threadPoolKey = pickKey(root, "controller-thread-pool", "controller_thread_pool");
    if (!root.hasChild(threadPoolKey))
    {
        throw Error(makeMissingFieldMessage(threadPoolKey));
    }

    const auto threadPoolNodes = root.subConfig(threadPoolKey);
    if (threadPoolNodes.empty())
    {
        throw Error(makeMissingFieldMessage(threadPoolKey));
    }

    const auto& threadPoolNode = threadPoolNodes.front();
    if (!threadPoolNode.raw().has_val())
    {
        throw Error(threadPoolKey + " must be a scalar");
    }

    threadPoolNode >> controllerThreadPoolSize_;
    if (controllerThreadPoolSize_ <= 0)
    {
        throw Error(threadPoolKey + " must be greater than zero");
    }
}

void MLDPPVXSControllerConfig::parsePool(const ::mldp_pvxs_driver::config::Config& root)
{
    const auto poolKey = pickKey(root, "mldp-pool", "mldp_pool");
    if (!root.hasChild(poolKey))
    {
        throw Error(makeMissingFieldMessage(poolKey));
    }

    const auto poolNodes = root.subConfig(poolKey);
    if (poolNodes.empty())
    {
        throw Error(makeMissingFieldMessage(poolKey));
    }

    try
    {
        pool_ = util::pool::MLDPGrpcPoolConfig(poolNodes.front());
    }
    catch (const util::pool::MLDPGrpcPoolConfig::Error& e)
    {
        throw Error(e.what());
    }
}

void MLDPPVXSControllerConfig::parseReaders(const ::mldp_pvxs_driver::config::Config& root)
{
    readerConfigs_.clear();
    readerEntries_.clear();

    if (!root.hasChild("reader"))
    {
        return;
    }

    if (!root.isSequence("reader"))
    {
        throw Error("reader must be a sequence");
    }

    const auto readerBlocks = root.subConfig("reader");
    for (const auto& readerBlock : readerBlocks)
    {
        if (!readerBlock.raw().is_map())
        {
            throw Error("Each entry in reader must be a map");
        }

        bool handledType = false;

        const std::pair<std::string, std::string> supportedTypes[] = {
            {"epics-pvxs", "epics-pvxs"},
            {"epics-base", "epics-base"},
            {"epics-archiver", "epics-archiver"},
        };

        for (const auto& [key, typeName] : supportedTypes)
        {
            if (readerBlock.hasChild(key))
            {
                handledType = true;
                if (!readerBlock.isSequence(key))
                {
                    throw Error("reader[]." + key + " must be a sequence");
                }
                const auto nodes = readerBlock.subConfig(key);
                for (const auto& node : nodes)
                {
                    readerConfigs_.push_back(node);
                    readerEntries_.push_back({typeName, node});
                }
            }
        }

        if (!handledType)
        {
            throw Error("reader entry does not specify a supported type (expected 'epics-pvxs', 'epics-base', or 'epics-archiver')");
        }
    }
}

void MLDPPVXSControllerConfig::parseMetrics(const ::mldp_pvxs_driver::config::Config& root)
{
    metricsConfig_.reset();
    if (!root.hasChild("metrics"))
    {
        metricsConfig_.emplace(Config()); // empty config
        return;
    }

    const auto metricsNodes = root.subConfig("metrics");
    if (metricsNodes.empty())
    {
        throw Error("metrics block is present but empty");
    }

    metricsConfig_.emplace(metricsNodes.front());
}

void MLDPPVXSControllerConfig::parseStreamLimits(const ::mldp_pvxs_driver::config::Config& root)
{
    const auto maxBytesKey = pickKey(root, "controller-stream-max-bytes", "controller_stream_max_bytes");
    if (root.hasChild(maxBytesKey))
    {
        const auto nodes = root.subConfig(maxBytesKey);
        if (nodes.empty() || !nodes.front().raw().has_val())
        {
            throw Error(maxBytesKey + " must be a scalar");
        }
        int value = 0;
        nodes.front() >> value;
        if (value <= 0)
        {
            throw Error(maxBytesKey + " must be greater than zero");
        }
        controllerStreamMaxBytes_ = static_cast<std::size_t>(value);
    }

    const auto maxAgeMsKey = pickKey(root, "controller-stream-max-age-ms", "controller_stream_max_age_ms");
    if (root.hasChild(maxAgeMsKey))
    {
        const auto nodes = root.subConfig(maxAgeMsKey);
        if (nodes.empty() || !nodes.front().raw().has_val())
        {
            throw Error(maxAgeMsKey + " must be a scalar");
        }
        int value = 0;
        nodes.front() >> value;
        if (value <= 0)
        {
            throw Error(maxAgeMsKey + " must be greater than zero");
        }
        controllerStreamMaxAge_ = std::chrono::milliseconds(value);
    }

}
