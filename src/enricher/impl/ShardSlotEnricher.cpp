//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <enricher/impl/ShardSlotEnricher.h>

#include <cstdio>
#include <stdexcept>

namespace mldp_pvxs_driver::enricher {

ShardSlotEnricher::ShardSlotEnricher(const config::Config& config)
{
    configure(config);
}

void ShardSlotEnricher::configure(const config::Config& config)
{
    const int count = config.getInt("num-shards", 6);
    if (count < 1 || count > 65536)
        throw std::runtime_error("shard-slot 'num-shards' must be in range 1..65536");
    num_shards_ = static_cast<std::size_t>(count);
}

bool ShardSlotEnricher::enrich(util::bus::IDataBus::EventBatch& batch) noexcept
{
    if (!util::bus::isTimeSeries(batch))
        return true;

    for (auto& frame : std::get<util::bus::TimeSeriesPayload>(batch.payload).frames)
    {
        for (auto& column : frame.columns)
        {
            if (column.metadata.contains("shardSlot"))
                continue;

            auto [slot, inserted] = slots_.emplace(column.name, 0);
            if (inserted)
            {
                const auto shard = next_shard_++ % num_shards_;
                const auto lower = static_cast<uint32_t>((65536ULL * shard) / num_shards_);
                const auto upper = static_cast<uint32_t>((65536ULL * (shard + 1)) / num_shards_ - 1);
                slot->second = static_cast<uint16_t>(std::uniform_int_distribution<uint32_t>(lower, upper)(rng_));
            }

            char formatted_slot[6];
            std::snprintf(formatted_slot, sizeof(formatted_slot), "%05u", static_cast<unsigned>(slot->second));
            column.metadata["shardSlot"] = formatted_slot;
        }
    }
    return true;
}

} // namespace mldp_pvxs_driver::enricher
