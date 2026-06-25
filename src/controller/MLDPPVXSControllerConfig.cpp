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
#include <writer/WriterFactory.h>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::writer;

static constexpr auto QueryableKey = "queryable";

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

const std::vector<std::pair<std::string, Config>>&
MLDPPVXSControllerConfig::processorEntries() const
{
    return processorEntries_;
}

const std::optional<MetricsConfig>& MLDPPVXSControllerConfig::metricsConfig() const
{
    return metricsConfig_;
}

const std::vector<RouteFilterEntry>&
MLDPPVXSControllerConfig::routeEntries() const
{
    return routeEntries_;
}

void MLDPPVXSControllerConfig::parse(const ::mldp_pvxs_driver::config::Config& root)
{
    name_ = root.get(NameKey, "default");
    parseWriter(root);
    parseReaders(root);
    parseProcessors(root);
    parseMetrics(root);
    parseRouting(root);
    parseQueryables(root);

    queue_capacity_   = static_cast<std::size_t>(root.getInt("queue_capacity", static_cast<int>(queue_capacity_)));
    push_timeout_ms_  = static_cast<std::uint32_t>(root.getInt("push_timeout_ms", static_cast<int>(push_timeout_ms_)));

    valid_ = true;
}

void MLDPPVXSControllerConfig::parseWriter(const ::mldp_pvxs_driver::config::Config& root)
{
    writerEntries_.clear();

    if (!root.hasChild(WriterKey))
    {
        throw Error("'writer' block is missing; configure at least one writer under writer.<type>");
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

    for (const auto& typeName : WriterFactory::registeredTypes())
    {
        if (!writerNode.hasChild(typeName))
            continue;

        const auto items = writerNode.subConfig(typeName);
        for (const auto& item : items)
        {
            writerEntries_.push_back({typeName, item});
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

    const auto readerNodes = root.subConfig(ReaderKey);
    if (readerNodes.empty())
    {
        return;
    }

    const auto& readerNode     = readerNodes.front();
    const auto  registeredTypes = mldp_pvxs_driver::reader::ReaderFactory::registeredTypes();

    for (const auto& typeName : registeredTypes)
    {
        if (!readerNode.hasChild(typeName))
            continue;

        if (!readerNode.isSequence(typeName))
        {
            throw Error("reader." + typeName + " must be a sequence");
        }

        const auto nodes = readerNode.subConfig(typeName);
        for (const auto& node : nodes)
        {
            readerConfigs_.push_back(node);
            readerEntries_.push_back({typeName, node});
        }
    }
}

void MLDPPVXSControllerConfig::parseProcessors(const ::mldp_pvxs_driver::config::Config& root)
{
    processorEntries_.clear();

    if (!root.hasChild("processors"))
    {
        return;
    }

    if (!root.isSequence("processors"))
    {
        throw Error("processors must be a sequence");
    }

    for (const auto& processorNode : root.subConfig("processors"))
    {
        const auto type = processorNode.get("type", "");
        if (type.empty())
        {
            throw Error("processor entry missing 'type' field");
        }
        processorEntries_.push_back({type, processorNode});
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
    const auto  rawNode = routingNode.raw();

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

        std::vector<std::string> includePatterns;
        if (writerCfg.hasChild("include"))
        {
            if (!writerCfg.isSequence("include"))
            {
                throw Error("routing entry '" + writerName + "': 'include' must be a sequence");
            }
            for (const auto& node : writerCfg.subConfig("include"))
            {
                std::string pat;
                node >> pat;
                includePatterns.push_back(std::move(pat));
            }
        }

        std::vector<std::string> excludePatterns;
        if (writerCfg.hasChild("exclude"))
        {
            if (!writerCfg.isSequence("exclude"))
            {
                throw Error("routing entry '" + writerName + "': 'exclude' must be a sequence");
            }
            for (const auto& node : writerCfg.subConfig("exclude"))
            {
                std::string pat;
                node >> pat;
                excludePatterns.push_back(std::move(pat));
            }
        }

        routeEntries_.push_back({writerName, std::move(fromReaders),
                                 std::move(includePatterns), std::move(excludePatterns)});
    }
}

void MLDPPVXSControllerConfig::parseQueryables(const ::mldp_pvxs_driver::config::Config& root)
{
    queryable_entries_.clear();

    if (!root.hasChild(QueryableKey))
    {
        return;
    }

    if (!root.isSequence(QueryableKey))
    {
        throw Error("queryable must be a sequence");
    }

    for (const auto& node : root.subConfig(QueryableKey))
    {
        const auto type = node.get("type", "");
        if (type.empty())
        {
            throw Error("queryable entry missing 'type' field");
        }
        queryable_entries_.push_back({type, node});
    }
}
