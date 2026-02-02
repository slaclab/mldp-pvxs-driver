//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/EpicsReaderConfig.h>

#include <algorithm>
#include <utility>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader::impl::epics;

namespace {
std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

void requireScalarChild(const Config& cfg, const std::string& key, const std::string& context)
{
    const auto raw = cfg.raw();
    if (!raw.has_child(key.c_str()))
    {
        return;
    }

    const auto child = raw[key.c_str()];
    if (!child.has_val())
    {
        throw EpicsReaderConfig::Error(context + "." + key + " must be a scalar");
    }
}

std::optional<EpicsReaderConfig::PVConfig::NTTableRowTimestampOptions> parseNtTableRowTsOption(
    const std::string& pvName,
    const Config& optionCfg)
{
    const std::string optionContext = "pvs[].option (pv '" + pvName + "')";

    if (!optionCfg.valid())
    {
        return std::nullopt;
    }

    // Only interpret the subtree when it advertises a supported type.
    if (!optionCfg.hasChild("type"))
    {
        return std::nullopt;
    }

    requireScalarChild(optionCfg, "type", optionContext);
    const auto type = optionCfg.get("type");
    if (toLower(type) != "nttable-rowts")
    {
        return std::nullopt;
    }

    EpicsReaderConfig::PVConfig::NTTableRowTimestampOptions opts;

    requireScalarChild(optionCfg, "tsSeconds", optionContext);
    requireScalarChild(optionCfg, "tsNanos", optionContext);
    opts.tsSecondsField = optionCfg.get("tsSeconds", opts.tsSecondsField);
    opts.tsNanosField = optionCfg.get("tsNanos", opts.tsNanosField);
    if (opts.tsSecondsField.empty())
    {
        throw EpicsReaderConfig::Error(optionContext + ".tsSeconds must not be empty");
    }
    if (opts.tsNanosField.empty())
    {
        throw EpicsReaderConfig::Error(optionContext + ".tsNanos must not be empty");
    }

    if (optionCfg.hasChild("sourceName"))
    {
        throw EpicsReaderConfig::Error(optionContext + ".sourceName is not supported for type 'nttable-rowts'; source name always equals the column field name");
    }

    return opts;
}
} // namespace

EpicsReaderConfig::EpicsReaderConfig() = default;

EpicsReaderConfig::EpicsReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry)
{
    if (!readerEntry.valid())
    {
        throw Error("Reader entry is invalid");
    }

    parse(readerEntry);
}

bool EpicsReaderConfig::valid() const
{
    return valid_;
}

const std::string& EpicsReaderConfig::name() const
{
    return name_;
}

unsigned int EpicsReaderConfig::threadPoolSize() const
{
    return thread_pool_size_;
}

const std::vector<EpicsReaderConfig::PVConfig>& EpicsReaderConfig::pvs() const
{
    return pvs_;
}

const std::vector<std::string>& EpicsReaderConfig::pvNames() const
{
    return pvNames_;
}

void EpicsReaderConfig::parse(const Config& readerEntry)
{
    if (!readerEntry.hasChild("name"))
    {
        throw Error(makeMissingFieldMessage("name"));
    }

    const auto nameNodes = readerEntry.subConfig("name");
    if (nameNodes.empty())
    {
        throw Error(makeMissingFieldMessage("name"));
    }

    const auto& nameNode = nameNodes.front();
    if (!nameNode.raw().has_val())
    {
        throw Error("name must be a scalar");
    }

    nameNode >> name_;
    if (name_.empty())
    {
        throw Error("name must not be empty");
    }

    thread_pool_size_ = static_cast<unsigned int>(readerEntry.getInt("thread_pool", 2));

    if (!readerEntry.hasChild("pvs"))
    {
        return;
    }

    if (!readerEntry.isSequence("pvs"))
    {
        throw Error("pvs must be a sequence");
    }

    const auto pvNodes = readerEntry.subConfig("pvs");

    pvs_.clear();
    pvNames_.clear();
    pvs_.reserve(pvNodes.size());
    pvNames_.reserve(pvNodes.size());

    for (const auto& pvNode : pvNodes)
    {
        if (!pvNode.raw().is_map())
        {
            throw Error("Each entry in pvs must be a map");
        }

        if (!pvNode.hasChild("name"))
        {
            throw Error(makeMissingFieldMessage("pvs[].name"));
        }

        const auto pvNameNodes = pvNode.subConfig("name");
        if (pvNameNodes.empty())
        {
            throw Error(makeMissingFieldMessage("pvs[].name"));
        }

        const auto& pvNameNode = pvNameNodes.front();
        if (!pvNameNode.raw().has_val())
        {
            throw Error("pvs[].name must be a scalar");
        }

        std::string pvName;
        pvNameNode >> pvName;
        if (pvName.empty())
        {
            throw Error("pvs[].name must not be empty");
        }

        std::string option;
        std::optional<Config> optionConfig;
        std::optional<PVConfig::NTTableRowTimestampOptions> nttableRowTs;
        if (pvNode.hasChild("option"))
        {
            const auto optionNodes = pvNode.subConfig("option");
            if (optionNodes.empty())
            {
                throw Error(makeMissingFieldMessage("pvs[].option"));
            }

            const auto& optionNode = optionNodes.front();
            const auto  raw = optionNode.raw();
            if (raw.is_map() || raw.is_seq())
            {
                optionConfig = optionNode;
                nttableRowTs = parseNtTableRowTsOption(pvName, *optionConfig);
            }
            else if (raw.has_val())
            {
                optionNode >> option;
            }
            else
            {
                throw Error("pvs[].option must be a scalar or map");
            }

        }

        pvs_.push_back({std::move(pvName), std::move(option), optionConfig, nttableRowTs});
        pvNames_.push_back(pvs_.back().name);
    }

    valid_ = true;
}
