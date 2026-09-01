//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file   EpicsDSMetadataReaderConfig.cpp
 * @brief  Implementation of EpicsDSMetadataReaderConfig.
 * @author SLAC MLDP Team
 * @date   2025-01-01
 * @copyright Copyright (c) 2025 SLAC National Accelerator Laboratory
 */

#include <reader/impl/epics/shared/PvxsClientConfig.h>
#include <reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace mldp_pvxs_driver::reader::impl::epics_ds {

namespace {

    constexpr auto kPvsKey = "pvs";
    constexpr auto kPvNameKey = "name";
    constexpr auto kPvMetadataKey = "metadata";
    constexpr auto kPvShowColumnsKey = "pv-show-columns";
    constexpr auto kDefaultPvShowColumns = "dname,ename,etype,lname,ioc,scheme,z";

    std::string trim(std::string value)
    {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c)
                                            {
                                                return std::isspace(c) != 0;
                                            });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c)
                                           {
                                               return std::isspace(c) != 0;
                                           })
                              .base();
        if (first >= last)
            return {};
        return std::string(first, last);
    }

    std::vector<std::string> parseCommaSeparated(const std::string& raw)
    {
        std::vector<std::string> values;
        std::istringstream       stream(raw);
        std::string              token;
        while (std::getline(stream, token, ','))
        {
            auto trimmed = trim(token);
            if (!trimmed.empty())
                values.push_back(std::move(trimmed));
        }
        return values;
    }

} // namespace

static constexpr auto kNameKey = "name";
static constexpr auto kServiceKey = "service";
static constexpr auto kQueryKey = "query";
static constexpr auto kTimeoutSecKey = "timeout-sec";
static constexpr auto kSourceNameColumnKey = "source-name-column";
static constexpr auto kTagsColumnKey = "tags-column";
static constexpr auto kShowColumnsKey = "show-columns";
static constexpr auto kRescanIntervalSecKey = "rescan-interval-sec";
static constexpr auto kWorkerThreadCountKey = "worker-thread-count";
static constexpr auto kMaxQueueDepthKey = "max-queue-depth";

EpicsDSMetadataReaderConfig::EpicsDSMetadataReaderConfig(const config::Config& cfg)
{
    parse(cfg);
}

void EpicsDSMetadataReaderConfig::parse(const config::Config& cfg)
{
    try
    {
        epics::PvxsClientConfig::validate(cfg, "epics-ds-metadata");
    }
    catch (const epics::PvxsClientConfig::Error& error)
    {
        throw Error(error.what());
    }

    if (!cfg.hasChild(kNameKey))
        throw Error("epics-ds-metadata reader: 'name' is required");
    name_ = cfg.get(kNameKey);
    if (name_.empty())
        throw Error("epics-ds-metadata reader: 'name' must not be empty");

    service_ = cfg.get(kServiceKey, "ds");
    query_ = cfg.get(kQueryKey, "%");
    timeout_sec_ = cfg.getDouble(kTimeoutSecKey, 5.0);
    source_name_column_ = cfg.get(kSourceNameColumnKey, "channelName");
    tags_column_ = cfg.get(kTagsColumnKey, "");
    show_columns_ = cfg.get(kShowColumnsKey, "");
    rescan_interval_sec_ = cfg.getDouble(kRescanIntervalSecKey, 0.0);
    pvs_.clear();
    pv_show_columns_.clear();

    const int wtc = cfg.getInt(kWorkerThreadCountKey, 1);
    const int mqd = cfg.getInt(kMaxQueueDepthKey, 16);

    if (timeout_sec_ <= 0.0)
        throw Error("epics-ds-metadata reader: 'timeout-sec' must be positive");
    if (rescan_interval_sec_ < 0.0)
        throw Error("epics-ds-metadata reader: 'rescan-interval-sec' must be >= 0");
    if (wtc < 1 || wtc > 64)
        throw Error("epics-ds-metadata reader: 'worker-thread-count' must be 1..64");
    if (mqd < 1 || mqd > 1024)
        throw Error("epics-ds-metadata reader: 'max-queue-depth' must be 1..1024");

    if (!cfg.hasChild(kPvsKey))
        throw Error("epics-ds-metadata reader: 'pvs' is required");
    if (!cfg.isSequence(kPvsKey))
        throw Error("epics-ds-metadata reader: 'pvs' must be a sequence");

    for (const auto& pvCfg : cfg.subConfig(kPvsKey))
    {
        if (!pvCfg.hasChild(kPvNameKey))
            throw Error("epics-ds-metadata reader: each 'pvs' entry requires 'name'");

        PVEntry entry;
        entry.name = trim(pvCfg.get(kPvNameKey));
        if (entry.name.empty())
            throw Error("epics-ds-metadata reader: 'pvs[].name' must not be empty");

        if (pvCfg.hasChild(kPvMetadataKey))
        {
            const auto metadataNodes = pvCfg.subConfig(kPvMetadataKey);
            if (!metadataNodes.empty())
            {
                const auto raw = metadataNodes.front().raw();
                if (!raw.invalid() && !raw.is_map())
                    throw Error("epics-ds-metadata reader: 'pvs[].metadata' must be a map");

                if (!raw.invalid() && raw.is_map())
                {
                    for (const auto child : raw.children())
                    {
                        if (!child.has_key() || !child.has_val())
                            continue;
                        std::string key{child.key().str, child.key().len};
                        std::string value;
                        child >> value;
                        entry.metadata.emplace(std::move(key), std::move(value));
                    }
                }
            }
        }

        pvs_.push_back(std::move(entry));
    }
    if (pvs_.empty())
        throw Error("epics-ds-metadata reader: 'pvs' must contain at least one entry");

    pv_show_columns_ = parseCommaSeparated(cfg.get(kPvShowColumnsKey, kDefaultPvShowColumns));
    if (pv_show_columns_.empty())
        pv_show_columns_ = parseCommaSeparated(kDefaultPvShowColumns);
    std::set<std::string> seen;
    for (const auto& column : pv_show_columns_)
    {
        if (!seen.insert(column).second)
            throw Error("epics-ds-metadata reader: duplicate 'pv-show-columns' value '" + column + "'");
    }

    worker_thread_count_ = static_cast<std::size_t>(wtc);
    max_queue_depth_ = static_cast<std::size_t>(mqd);
}

} // namespace mldp_pvxs_driver::reader::impl::epics_ds
