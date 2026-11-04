//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderConfig.h>

#include <charconv>

using namespace mldp_pvxs_driver::reader::impl::epics;
using mldp_pvxs_driver::config::Config;

namespace {

using TableOptions = EpicsPVXSBsasTableReaderConfig::TableOptions;
using Generation = EpicsPVXSBsasTableReaderConfig::Generation;

/// Read a scalar string key; throws when present but not a scalar or empty.
void readName(const Config& cfg, const char* key, const std::string& context, std::string& out)
{
    if (!cfg.hasChild(key))
    {
        return;
    }
    if (!cfg.raw()[key].has_val())
    {
        throw EpicsReaderConfig::Error(context + "." + key + " must be a scalar");
    }
    out = cfg.get(key);
    if (out.empty())
    {
        throw EpicsReaderConfig::Error(context + "." + key + " must not be empty");
    }
}

/// Read a non-negative integer key; throws on any other value.
void readCount(const Config& cfg, const char* key, const std::string& context, std::size_t& out)
{
    if (!cfg.hasChild(key))
    {
        return;
    }
    const auto        raw = cfg.get(key);
    unsigned long long value = 0;
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (!cfg.raw()[key].has_val() || raw.empty() || ec != std::errc{} || ptr != raw.data() + raw.size())
    {
        throw EpicsReaderConfig::Error(context + "." + key + " must be a non-negative integer");
    }
    out = static_cast<std::size_t>(value);
}

/// Apply the table keys found in @p cfg on top of @p opts.
void readTableOptions(const Config& cfg, Generation generation, const std::string& context,
                      std::string_view readerType, TableOptions& opts)
{
    if (cfg.hasChild(ColumnBatchSizeKey))
    {
        throw EpicsReaderConfig::Error(context + "." + std::string(ColumnBatchSizeKey) + " is not supported by reader type '" +
                                       std::string(readerType) + "'; use " + DataBatchesPerPushKey);
    }
    readCount(cfg, RowsPerDataBatchKey, context, opts.rowsPerDataBatch);
    readCount(cfg, PvsPerDataBatchKey, context, opts.pvsPerDataBatch);
    readCount(cfg, DataBatchesPerPushKey, context, opts.dataBatchesPerPush);
    readName(cfg, TsSecondsKey, context, opts.tsSecondsField);
    readName(cfg, TsNanosKey, context, opts.tsNanosField);

    if (generation == Generation::Gen2)
    {
        readName(cfg, PidFieldKey, context, opts.pidField);
        readName(cfg, ValueSuffixKey, context, opts.valueSuffix);
    }
    else
    {
        for (const char* key : {PidFieldKey, ValueSuffixKey})
        {
            if (cfg.hasChild(key))
            {
                throw EpicsReaderConfig::Error(context + "." + key + " is not supported by reader type '" +
                                               std::string(readerType) + "'; it applies to 'epics-pvxs-bsas-gen2' only");
            }
        }
    }
}

} // namespace

EpicsPVXSBsasTableReaderConfig::EpicsPVXSBsasTableReaderConfig(const Config&    readerEntry,
                                                               Generation       generation,
                                                               std::string_view readerType)
    : EpicsPVXSReaderConfig(readerEntry, std::string(readerType))
    , generation_(generation)
{
    readTableOptions(readerEntry, generation_, std::string(readerType), readerType, defaults_);

    for (const auto& pv : pvs())
    {
        const std::string context = "pvs[].option (pv '" + pv.name + "')";
        if (!pv.option.empty())
        {
            throw Error(context + " must be a map for reader type '" + std::string(readerType) + "'");
        }
        if (!pv.optionConfig.has_value())
        {
            continue;
        }
        const auto& optionCfg = *pv.optionConfig;
        if (!optionCfg.raw().is_map())
        {
            throw Error(context + " must be a map for reader type '" + std::string(readerType) + "'");
        }
        if (optionCfg.hasChild(OptionTypeKey))
        {
            throw Error(context + "." + std::string(OptionTypeKey) + " is not supported by reader type '" +
                        std::string(readerType) + "'; the table type is implied by the reader type");
        }

        TableOptions opts = defaults_;
        readTableOptions(optionCfg, generation_, context, readerType, opts);
        per_pv_.emplace(pv.name, std::move(opts));
    }
}

const EpicsPVXSBsasTableReaderConfig::TableOptions& EpicsPVXSBsasTableReaderConfig::optionsFor(const std::string& pvName) const
{
    const auto it = per_pv_.find(pvName);
    return it != per_pv_.end() ? it->second : defaults_;
}
