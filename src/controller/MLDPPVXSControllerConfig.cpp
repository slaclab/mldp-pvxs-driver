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
#include <reader/ReaderFactory.h>
#include <writer/WriterConfig.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::writer;

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
    return pool_.value();
}

const std::string& MLDPPVXSControllerConfig::providerName() const
{
    return pool_.value().providerName();
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

const std::vector<std::pair<std::string, Config>>&
MLDPPVXSControllerConfig::writerEntries() const
{
    return writerEntries_;
}

const std::optional<MetricsConfig>& MLDPPVXSControllerConfig::metricsConfig() const
{
    return metricsConfig_;
}

const mldp_pvxs_driver::writer::WriterConfig& MLDPPVXSControllerConfig::writerConfig() const
{
    return writerConfig_;
}

void MLDPPVXSControllerConfig::parse(const ::mldp_pvxs_driver::config::Config& root)
{
    parseThreadPool(root);
    parseStreamLimits(root);
    parseWriter(root);
    parsePool(root);
    parseReaders(root);
    parseMetrics(root);
    valid_ = true;
}

void MLDPPVXSControllerConfig::parseThreadPool(const ::mldp_pvxs_driver::config::Config& root)
{
    using namespace mldp_pvxs_driver::controller;
    if (!root.hasChild(ControllerThreadPoolKey))
    {
        controllerThreadPoolSize_ = 1;
        return;
    }

    const auto threadPoolNodes = root.subConfig(ControllerThreadPoolKey);
    if (threadPoolNodes.empty())
    {
        controllerThreadPoolSize_ = 1;
        return;
    }

    const auto& threadPoolNode = threadPoolNodes.front();
    if (!threadPoolNode.raw().has_val())
    {
        throw Error(std::string(ControllerThreadPoolKey) + " must be a scalar");
    }

    threadPoolNode >> controllerThreadPoolSize_;
    if (controllerThreadPoolSize_ <= 0)
    {
        throw Error(std::string(ControllerThreadPoolKey) + " must be greater than zero");
    }
}

void MLDPPVXSControllerConfig::parseWriter(const ::mldp_pvxs_driver::config::Config& root)
{
    writerEntries_.clear();

    if (root.hasChild(WriterKey))
    {
        const auto writerNodes = root.subConfig(WriterKey);
        if (writerNodes.empty())
        {
            throw Error("writer block is present but empty");
        }
        const auto& writerNode = writerNodes.front();
        try
        {
            writerConfig_ = WriterConfig::parse(writerNode, root);
        }
        catch (const WriterConfig::Error& e)
        {
            throw Error(e.what());
        }
        catch (const std::invalid_argument& e)
        {
            throw Error(e.what());
        }

        // Build writerEntries_: gRPC writer uses the root node (pool keys live there);
        // HDF5 writer uses its own sub-node.
        if (writerConfig_.grpcEnabled)
        {
            writerEntries_.push_back({"grpc", root});
        }
        if (writerConfig_.hdf5Enabled && writerNode.hasChild(WriterHdf5Key))
        {
            const auto hdf5Nodes = writerNode.subConfig(WriterHdf5Key);
            if (!hdf5Nodes.empty())
            {
                writerEntries_.push_back({"hdf5", hdf5Nodes.front()});
            }
        }
    }
    else
    {
        // No writer block → default: gRPC enabled, parsed from root
        writerConfig_ = WriterConfig{};
        writerConfig_.grpcEnabled = true;
        writerConfig_.hdf5Enabled = false;
        try
        {
            writerConfig_.grpcConfig = MLDPGrpcWriterConfig::parse(root);
        }
        catch (const MLDPGrpcWriterConfig::Error& e)
        {
            throw Error(e.what());
        }
        writerEntries_.push_back({"grpc", root});
    }
}

void MLDPPVXSControllerConfig::parsePool(const ::mldp_pvxs_driver::config::Config& root)
{
    using namespace mldp_pvxs_driver::controller;
    if (!writerConfig_.grpcEnabled)
    {
        // gRPC writer not active — pool not required
        return;
    }
    if (!root.hasChild(MldpPoolKey))
    {
        throw Error("writer.grpc is enabled but '" + std::string(MldpPoolKey) + "' block is missing");
    }

    const auto poolNodes = root.subConfig(MldpPoolKey);
    if (poolNodes.empty())
    {
        throw Error(makeMissingFieldMessage(MldpPoolKey));
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
    using namespace mldp_pvxs_driver::controller;
    readerConfigs_.clear();
    readerEntries_.clear();

    if (!root.hasChild(ReaderKey))
    {
        return;
    }

    if (!root.isSequence(ReaderKey))
    {
        throw Error("reader must be a sequence");
    }

    const auto registeredTypes = mldp_pvxs_driver::reader::ReaderFactory::registeredTypes();

    const auto readerBlocks = root.subConfig(ReaderKey);
    for (const auto& readerBlock : readerBlocks)
    {
        if (!readerBlock.raw().is_map())
        {
            throw Error("Each entry in reader must be a map");
        }

        bool handledType = false;

        for (const auto& typeName : registeredTypes)
        {
            if (readerBlock.hasChild(typeName))
            {
                handledType = true;
                if (!readerBlock.isSequence(typeName))
                {
                    throw Error("reader[]." + typeName + " must be a sequence");
                }
                const auto nodes = readerBlock.subConfig(typeName);
                for (const auto& node : nodes)
                {
                    readerConfigs_.push_back(node);
                    readerEntries_.push_back({typeName, node});
                }
            }
        }

        if (!handledType)
        {
            std::string available;
            for (size_t i = 0; i < registeredTypes.size(); ++i)
            {
                if (i > 0)
                    available += ", ";
                available += "'" + registeredTypes[i] + "'";
            }
            throw Error("reader entry does not specify a registered type (expected " + available + ")");
        }
    }
}

void MLDPPVXSControllerConfig::parseMetrics(const ::mldp_pvxs_driver::config::Config& root)
{
    using namespace mldp_pvxs_driver::controller;
    metricsConfig_.reset();
    if (!root.hasChild(MetricsKey))
    {
        metricsConfig_.emplace(Config()); // empty config
        return;
    }

    const auto metricsNodes = root.subConfig(MetricsKey);
    if (metricsNodes.empty())
    {
        throw Error("metrics block is present but empty");
    }

    metricsConfig_.emplace(metricsNodes.front());
}

void MLDPPVXSControllerConfig::parseStreamLimits(const ::mldp_pvxs_driver::config::Config& root)
{
    using namespace mldp_pvxs_driver::controller;
    if (root.hasChild(ControllerStreamMaxBytesKey))
    {
        const auto nodes = root.subConfig(ControllerStreamMaxBytesKey);
        if (nodes.empty() || !nodes.front().raw().has_val())
        {
            throw Error(std::string(ControllerStreamMaxBytesKey) + " must be a scalar");
        }
        int value = 0;
        nodes.front() >> value;
        if (value <= 0)
        {
            throw Error(std::string(ControllerStreamMaxBytesKey) + " must be greater than zero");
        }
        controllerStreamMaxBytes_ = static_cast<std::size_t>(value);
    }

    if (root.hasChild(ControllerStreamMaxAgeMsKey))
    {
        const auto nodes = root.subConfig(ControllerStreamMaxAgeMsKey);
        if (nodes.empty() || !nodes.front().raw().has_val())
        {
            throw Error(std::string(ControllerStreamMaxAgeMsKey) + " must be a scalar");
        }
        int value = 0;
        nodes.front() >> value;
        if (value <= 0)
        {
            throw Error(std::string(ControllerStreamMaxAgeMsKey) + " must be greater than zero");
        }
        controllerStreamMaxAge_ = std::chrono::milliseconds(value);
    }
}
