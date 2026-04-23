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
#include <controller/RouteTable.h>
#include <reader/ReaderFactory.h>
#include <writer/WriterConfig.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
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

const std::string& MLDPPVXSControllerConfig::name() const
{
    return name_;
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

const std::vector<RouteTable::RouteEntry>&
MLDPPVXSControllerConfig::routeEntries() const
{
    return routeEntries_;
}

void MLDPPVXSControllerConfig::parse(const ::mldp_pvxs_driver::config::Config& root)
{
    name_ = root.get(NameKey, "default");
    parseWriter(root);
    parseReaders(root);
    parseMetrics(root);
    parseRouting(root);
    valid_ = true;
}

void MLDPPVXSControllerConfig::parseWriter(const ::mldp_pvxs_driver::config::Config& root)
{
    writerEntries_.clear();

    if (!root.hasChild(WriterKey))
    {
        throw Error("'writer' block is missing; configure at least one writer under writer.mldp or writer.hdf5");
    }

    const auto writerNodes = root.subConfig(WriterKey);
    if (writerNodes.empty())
    {
        throw Error("writer block is present but empty");
    }
    const auto& writerNode = writerNodes.front();

    try
    {
        WriterConfig::validate(writerNode);
    }
    catch (const WriterConfig::Error& e)
    {
        throw Error(e.what());
    }
    catch (const std::invalid_argument& e)
    {
        throw Error(e.what());
    }

    // Build one writerEntry per configured instance (sequence items).
    // The config node passed to the factory is the per-instance map node.
    if (writerNode.hasChild(WriterMldpKey))
    {
        const auto mldpItems = writerNode.subConfig(WriterMldpKey);
        for (const auto& item : mldpItems)
        {
            writerEntries_.push_back({"mldp", item});
        }
    }
    if (writerNode.hasChild(WriterHdf5Key))
    {
        const auto hdf5Items = writerNode.subConfig(WriterHdf5Key);
        for (const auto& item : hdf5Items)
        {
            writerEntries_.push_back({"hdf5", item});
        }
    }
}

void MLDPPVXSControllerConfig::parseReaders(const ::mldp_pvxs_driver::config::Config& root)
{
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

void MLDPPVXSControllerConfig::parseRouting(const ::mldp_pvxs_driver::config::Config& root)
{
    routeEntries_.clear();

    if (!root.hasChild(RoutingKey))
    {
        return; // no routing = all-to-all
    }

    const auto routingNodes = root.subConfig(RoutingKey);
    if (routingNodes.empty())
    {
        return;
    }
    const auto& routingNode = routingNodes.front();
    const auto  rawNode     = routingNode.raw();

    if (!rawNode.is_map())
    {
        throw Error("routing must be a map");
    }

    for (const auto& child : rawNode)
    {
        if (!child.has_key())
        {
            throw Error("routing entry must have a key (writer name)");
        }

        std::string writerName;
        c4::from_chars(child.key(), &writerName);

        if (!routingNode.hasChild(writerName))
        {
            throw Error("routing entry '" + writerName + "' not accessible");
        }

        const auto writerNodes = routingNode.subConfig(writerName);
        if (writerNodes.empty())
        {
            throw Error("routing entry '" + writerName + "' is empty");
        }
        const auto& writerCfg = writerNodes.front();

        if (!writerCfg.hasChild("from"))
        {
            throw Error("routing entry '" + writerName + "' must have a 'from' sequence");
        }

        if (!writerCfg.isSequence("from"))
        {
            throw Error("routing entry '" + writerName + "': 'from' must be a sequence");
        }

        std::vector<std::string> fromReaders;
        const auto               fromNodes = writerCfg.subConfig("from");
        for (const auto& fromNode : fromNodes)
        {
            std::string readerName;
            fromNode >> readerName;
            fromReaders.push_back(std::move(readerName));
        }

        routeEntries_.push_back({writerName, std::move(fromReaders)});
    }
}
