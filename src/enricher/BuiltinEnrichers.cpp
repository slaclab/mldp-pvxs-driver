//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <enricher/BuiltinEnrichers.h>

#include <algorithm>
#include <cstdio>
#include <fnmatch.h>
#include <stdexcept>

using namespace mldp_pvxs_driver::enricher;
using namespace mldp_pvxs_driver::util::bus;

namespace {
std::unordered_map<std::string, std::string> requireMap(const mldp_pvxs_driver::config::Config& config, const std::string& key)
{
    if (!config.hasChild(key))
        throw std::runtime_error("enricher requires '" + key + "'");
    std::unordered_map<std::string, std::string> result;
    const auto nodes = config.subConfig(key);
    if (nodes.size() != 1 || !nodes.front().raw().is_map())
        throw std::runtime_error("enricher '" + key + "' must be a mapping");
    for (const auto& [name, value] : nodes.front().namedSubConfig())
    {
        std::string text;
        value >> text;
        result.emplace(name, std::move(text));
    }
    return result;
}
template <typename F> void forColumns(EventBatchStruct& batch, F&& fn)
{
    if (!isTimeSeries(batch)) return;
    for (auto& frame : std::get<TimeSeriesPayload>(batch.payload).frames)
        for (auto& col : frame.columns) fn(col);
}
} // namespace

void StaticMetadataEnricher::configure(const config::Config& config) { metadata_ = requireMap(config, "metadata"); }
bool StaticMetadataEnricher::enrich(IDataBus::EventBatch& batch) noexcept { for (const auto& [k,v] : metadata_) batch.metadata[k]=v; return true; }

void ColumnAttributesEnricher::configure(const config::Config& config)
{
    pattern_ = config.get("column-pattern");
    if (pattern_.empty()) throw std::runtime_error("column-attributes enricher requires 'column-pattern'");
    attributes_ = requireMap(config, "attributes");
}
bool ColumnAttributesEnricher::enrich(IDataBus::EventBatch& batch) noexcept
{
    forColumns(batch, [this](DataColumn& col) { if (fnmatch(pattern_.c_str(), col.name.c_str(), 0) == 0) for (const auto& [k,v] : attributes_) col.metadata[k]=v; });
    return true;
}

bool TimestampClampEnricher::enrich(IDataBus::EventBatch& batch) noexcept
{
    if (!isTimeSeries(batch)) return true;
    for (auto& frame : std::get<TimeSeriesPayload>(batch.payload).frames)
        for (auto& timestamp : frame.timestamps) timestamp.nanoseconds = std::min<uint64_t>(timestamp.nanoseconds, 999999999U);
    return true;
}

void ShardSlotEnricher::configure(const config::Config& config)
{
    const int count = config.getInt("num-shards", 6);
    if (count < 1 || count > 65536) throw std::runtime_error("shard-slot 'num-shards' must be in range 1..65536");
    num_shards_ = static_cast<std::size_t>(count);
}
bool ShardSlotEnricher::enrich(IDataBus::EventBatch& batch) noexcept
{
    forColumns(batch, [this](DataColumn& col) {
        if (col.metadata.find("shardSlot") != col.metadata.end()) return;
        auto [it, inserted] = slots_.emplace(col.name, 0);
        if (inserted) {
            const auto shard = next_shard_++ % num_shards_;
            const uint32_t lo = static_cast<uint32_t>((65536ULL * shard) / num_shards_);
            const uint32_t hi = static_cast<uint32_t>((65536ULL * (shard + 1)) / num_shards_ - 1);
            it->second = static_cast<uint16_t>(std::uniform_int_distribution<uint32_t>(lo, hi)(rng_));
        }
        char slot[6]; std::snprintf(slot, sizeof(slot), "%05u", static_cast<unsigned>(it->second)); col.metadata["shardSlot"] = slot;
    });
    return true;
}
