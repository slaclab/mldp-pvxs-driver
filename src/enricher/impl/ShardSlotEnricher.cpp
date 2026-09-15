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

#include <sqlite3.h>

#include <cstdio>
#include <stdexcept>

namespace mldp_pvxs_driver::enricher {

ShardSlotEnricher::ShardSlotEnricher(const config::Config& config)
{
    configure(config);
}

ShardSlotEnricher::~ShardSlotEnricher()
{
    if (db_)
        sqlite3_close(db_);
}

void ShardSlotEnricher::configure(const config::Config& config)
{
    const int count = config.getInt("num-shards", 6);
    if (count < 1 || count > 65536)
        throw std::runtime_error("shard-slot 'num-shards' must be in range 1..65536");
    num_shards_ = static_cast<std::size_t>(count);

    db_path_ = config.get("db-path");
    if (db_path_.empty())
        throw std::runtime_error("shard-slot enricher requires 'db-path'");

    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK)
    {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("shard-slot: cannot open db '" + db_path_ + "': " + msg);
    }

    // WAL mode for safe concurrent access across multiple driver instances.
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    // Wait up to 5 s before failing on a busy lock.
    sqlite3_busy_timeout(db_, 5000);

    const char* ddl =
        "CREATE TABLE IF NOT EXISTS shard_slots "
        "(source_name TEXT PRIMARY KEY, slot INTEGER NOT NULL)";
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, ddl, nullptr, nullptr, &errmsg) != SQLITE_OK)
    {
        std::string msg = errmsg;
        sqlite3_free(errmsg);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("shard-slot: DDL failed: " + msg);
    }

    // Load existing assignments into the in-memory cache.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT source_name, slot FROM shard_slots", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        slots_[name]     = static_cast<uint16_t>(sqlite3_column_int(stmt, 1));
    }
    sqlite3_finalize(stmt);
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
                slot->second     = static_cast<uint16_t>(std::uniform_int_distribution<uint32_t>(lower, upper)(rng_));

                // Persist new assignment; INSERT OR IGNORE so a race with another instance
                // never overwrites a row it already wrote.
                sqlite3_stmt* ins = nullptr;
                sqlite3_prepare_v2(db_,
                    "INSERT OR IGNORE INTO shard_slots(source_name, slot) VALUES(?,?)",
                    -1, &ins, nullptr);
                sqlite3_bind_text(ins, 1, column.name.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(ins, 2, slot->second);
                sqlite3_step(ins);
                sqlite3_finalize(ins);

                // If another instance beat us to this row, reload its value so both
                // processes stay in sync.
                sqlite3_stmt* sel = nullptr;
                sqlite3_prepare_v2(db_,
                    "SELECT slot FROM shard_slots WHERE source_name=?",
                    -1, &sel, nullptr);
                sqlite3_bind_text(sel, 1, column.name.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(sel) == SQLITE_ROW)
                    slot->second = static_cast<uint16_t>(sqlite3_column_int(sel, 0));
                sqlite3_finalize(sel);
            }

            char formatted_slot[6];
            std::snprintf(formatted_slot, sizeof(formatted_slot), "%05u", static_cast<unsigned>(slot->second));
            column.metadata["shardSlot"] = formatted_slot;
        }
    }
    return true;
}

} // namespace mldp_pvxs_driver::enricher
