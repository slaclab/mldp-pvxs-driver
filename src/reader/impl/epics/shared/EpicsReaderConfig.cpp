//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/shared/EpicsReaderConfig.h>

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
    const Config&      optionCfg)
{
    using namespace mldp_pvxs_driver::reader::impl::epics;

    const std::string optionContext = "pvs[].option (pv '" + pvName + "')";

    if (!optionCfg.valid())
    {
        return std::nullopt;
    }

    // Only interpret the subtree when it advertises a supported type.
    if (!optionCfg.hasChild(OptionTypeKey))
    {
        return std::nullopt;
    }

    requireScalarChild(optionCfg, OptionTypeKey, optionContext);
    const auto type = optionCfg.get(OptionTypeKey);
    if (toLower(type) != "slac-bsas-table")
    {
        return std::nullopt;
    }

    EpicsReaderConfig::PVConfig::NTTableRowTimestampOptions opts;

    requireScalarChild(optionCfg, TsSecondsKey, optionContext);
    requireScalarChild(optionCfg, TsNanosKey, optionContext);
    opts.tsSecondsField = optionCfg.get(TsSecondsKey, opts.tsSecondsField);
    opts.tsNanosField = optionCfg.get(TsNanosKey, opts.tsNanosField);
    if (opts.tsSecondsField.empty())
    {
        throw EpicsReaderConfig::Error(optionContext + "." + std::string(TsSecondsKey) + " must not be empty");
    }
    if (opts.tsNanosField.empty())
    {
        throw EpicsReaderConfig::Error(optionContext + "." + std::string(TsNanosKey) + " must not be empty");
    }

    if (optionCfg.hasChild(SourceNameKey))
    {
        throw EpicsReaderConfig::Error(optionContext + "." + std::string(SourceNameKey) + " is not supported for type 'slac-bsas-table'; source name always equals the column field name");
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

unsigned int EpicsReaderConfig::monitorPollThreads() const
{
    return monitor_poll_threads_;
}

unsigned int EpicsReaderConfig::monitorPollIntervalMs() const
{
    return monitor_poll_interval_ms_;
}

std::size_t EpicsReaderConfig::columnBatchSize() const
{
    return column_batch_size_;
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
    using namespace mldp_pvxs_driver::reader::impl::epics;

    if (!readerEntry.hasChild(NameKey))
    {
        throw Error(makeMissingFieldMessage(NameKey));
    }

    const auto nameNodes = readerEntry.subConfig(NameKey);
    if (nameNodes.empty())
    {
        throw Error(makeMissingFieldMessage(NameKey));
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

    thread_pool_size_ = static_cast<unsigned int>(readerEntry.getInt(ThreadPoolKey, 1));
    column_batch_size_ = static_cast<std::size_t>(readerEntry.getInt(ColumnBatchSizeKey, 50));
    monitor_poll_threads_ = static_cast<unsigned int>(readerEntry.getInt(MonitorPollThreadsKey, 2));
    monitor_poll_interval_ms_ = static_cast<unsigned int>(readerEntry.getInt(MonitorPollIntervalMsKey, 5));

    if (readerEntry.hasChild(BackendKey))
    {
        throw Error("backend is not supported; choose epics-pvxs or epics-base reader type");
    }

    if (!readerEntry.hasChild(PvsKey))
    {
        return;
    }

    if (!readerEntry.isSequence(PvsKey))
    {
        throw Error("pvs must be a sequence");
    }

    const auto pvNodes = readerEntry.subConfig(PvsKey);

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

        if (!pvNode.hasChild(PvNameKey))
        {
            throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[].name"));
        }

        const auto pvNameNodes = pvNode.subConfig(PvNameKey);
        if (pvNameNodes.empty())
        {
            throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[].name"));
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

        std::string                                         option;
        std::optional<Config>                               optionConfig;
        std::optional<PVConfig::NTTableRowTimestampOptions> nttableRowTs;
        if (pvNode.hasChild(PvOptionKey))
        {
            const auto optionNodes = pvNode.subConfig(PvOptionKey);
            if (optionNodes.empty())
            {
                throw Error(makeMissingFieldMessage(std::string(PvsKey) + "[].option"));
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
