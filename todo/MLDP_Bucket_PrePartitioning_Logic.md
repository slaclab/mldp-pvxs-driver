# MLDP Bucket Pre-Partitioning Logic

## Goal

Distribute MLDP bucket documents across all MongoDB shards **before ingestion starts**, while keeping all documents for the same `pvname` on the same shard.

This avoids the startup behavior where all new bucket documents initially hit one shard and MongoDB later moves chunks in the background with the balancer.

For Gen1 BSAS NTTable ingestion, this is important because:

- Each Gen1 table contains ~1300 PV columns.
- Every PV in the table shares the same timestamp.
- Sharding by timestamp does not distribute writes.
- Sharding only after data is inserted can temporarily overload one shard.
- We want every PV to have stable shard affinity from the first insert.

## Core idea

Add one deterministic numeric field to each bucket document:

```json
{
  "pvName": "BPMS:DMPH:502:TMITBR",
  "source": "gen1_bsas",
  "shardSlot": 42137,
  "values": []
}
```

The field `shardSlot` is assigned once per `pvName+source` pair and stored in a metadata collection. From then on, every bucket document for that PV carries the same `shardSlot`.

```text
same pvName + source -> same shardSlot -> same MongoDB shard
```

## Slot assignment: reader-managed round-robin

Instead of a hash function, each reader **owns slot assignment for its source**.

When a reader encounters a `pvName` for the first time it:

1. Queries `dp.pvSlotAssignment` for `{pvName, source}`.
2. If the document exists, uses the stored `shardSlot`.
3. If not, **atomically** claims the next least-used slot from `dp.slotCounters` for that source and writes the assignment.

This guarantees perfectly uniform distribution regardless of the actual PV names.

### Metadata collections

**`dp.pvSlotAssignment`** — one document per PV per source:

```json
{
  "pvName": "BPMS:DMPH:502:TMITBR",
  "source": "gen1_bsas",
  "shardSlot": 42137,
  "assignedAt": ISODate("2026-07-04T00:00:00Z")
}
```

Index:
```javascript
db.pvSlotAssignment.createIndex({ pvName: 1, source: 1 }, { unique: true })
```

**`dp.slotCounters`** — one document per (source, slotId):

```json
{
  "source": "gen1_bsas",
  "slotId": 42137,
  "pvCount": 1
}
```

Index:
```javascript
db.slotCounters.createIndex({ source: 1, pvCount: 1, slotId: 1 })
```

### Slot assignment algorithm

Each reader keeps an in-memory cache: `unordered_map<string, uint32_t> pvSlotCache`.

On startup, the reader loads all existing assignments for its source into the cache:

```cpp
auto cursor = db["pvSlotAssignment"].find(
    make_document(kvp("source", source_name))
);
for (auto& doc : cursor) {
    pvSlotCache[doc["pvName"].get_string()] =
        doc["shardSlot"].get_int32();
}
```

When writing a bucket document for `pvName`:

```cpp
uint32_t getOrAssignSlot(const std::string& pvName, const std::string& source) {
    auto it = pvSlotCache.find(pvName);
    if (it != pvSlotCache.end()) {
        return it->second;
    }

    // Find the slot with the fewest PVs for this source.
    // Uses findOneAndUpdate with sort by pvCount ascending to atomically
    // increment the winner and return its slotId.
    auto result = db["slotCounters"].find_one_and_update(
        make_document(kvp("source", source)),
        make_document(kvp("$inc", make_document(kvp("pvCount", 1)))),
        findOneAndUpdateOptions{}
            .sort(make_document(kvp("pvCount", 1), kvp("slotId", 1)))
            .return_document(return_document::k_after)
    );

    uint32_t slot = result->view()["slotId"].get_int32();

    // Persist the assignment (upsert in case two readers race on the same PV).
    db["pvSlotAssignment"].update_one(
        make_document(kvp("pvName", pvName), kvp("source", source)),
        make_document(
            kvp("$setOnInsert", make_document(
                kvp("pvName", pvName),
                kvp("source", source),
                kvp("shardSlot", static_cast<int32_t>(slot)),
                kvp("assignedAt", bsoncxx::types::b_date{std::chrono::system_clock::now()})
            ))
        ),
        mongocxx::options::update{}.upsert(true)
    );

    // If two readers raced, read back the winner and return that slot.
    auto assigned = db["pvSlotAssignment"].find_one(
        make_document(kvp("pvName", pvName), kvp("source", source))
    );
    uint32_t finalSlot = assigned->view()["shardSlot"].get_int32();
    pvSlotCache[pvName] = finalSlot;
    return finalSlot;
}
```

If two readers race on the same new `pvName`, `$setOnInsert` ensures only one wins.
The loser re-reads the actual assignment and adjusts `slotCounters` with a compensating decrement if needed, or simply uses the winner's slot (slight imbalance of 1, acceptable).

## Slot space

Use 65,536 logical slots (0 to 65535). This is much larger than the number of shards so that the slot assignments stay stable when the cluster grows later.

Initialize `dp.slotCounters` before first ingestion:

```javascript
for (let i = 0; i < 65536; i++) {
  db.slotCounters.insertOne({ source: "gen1_bsas", slotId: i, pvCount: 0 });
}
```

With 1300 PVs in round-robin across 65,536 slots, 1300 slots will hold exactly one PV and the rest will stay empty. This is intentional: empty slots simply produce no documents and add no overhead.

## MongoDB shard key

Use range sharding on the explicit `shardSlot` field:

```javascript
sh.shardCollection(
  "dp.buckets",
  { shardSlot: 1 }
)
```

Range sharding lets us pre-assign slot ranges to specific shards before ingestion starts.

## Initial 6-shard layout

With 65,536 slots and 6 shards:

```text
rs0: 0     - 10921
rs1: 10922 - 21843
rs2: 21844 - 32765
rs3: 32766 - 43687
rs4: 43688 - 54609
rs5: 54610 - 65535
```

Expected distribution for ~1300 PVs:

```text
1300 PVs / 6 shards = ~217 PVs per shard
```

With round-robin assignment this is exact, not probabilistic.

## Pre-split workflow

```javascript
use dp

// Enable sharding on the database
sh.enableSharding("dp")

// Create the shard key index before sharding
db.buckets.createIndex({ shardSlot: 1 })

// Shard the collection
sh.shardCollection("dp.buckets", { shardSlot: 1 })

// Split into 6 ranges
sh.splitAt("dp.buckets", { shardSlot: 10922 })
sh.splitAt("dp.buckets", { shardSlot: 21844 })
sh.splitAt("dp.buckets", { shardSlot: 32766 })
sh.splitAt("dp.buckets", { shardSlot: 43688 })
sh.splitAt("dp.buckets", { shardSlot: 54610 })

// Assign each range to a shard
sh.moveRange("dp.buckets", { shardSlot: MinKey },  { shardSlot: 10922  }, "mldp-cluster-rs0")
sh.moveRange("dp.buckets", { shardSlot: 10922  },  { shardSlot: 21844  }, "mldp-cluster-rs1")
sh.moveRange("dp.buckets", { shardSlot: 21844  },  { shardSlot: 32766  }, "mldp-cluster-rs2")
sh.moveRange("dp.buckets", { shardSlot: 32766  },  { shardSlot: 43688  }, "mldp-cluster-rs3")
sh.moveRange("dp.buckets", { shardSlot: 43688  },  { shardSlot: 54610  }, "mldp-cluster-rs4")
sh.moveRange("dp.buckets", { shardSlot: 54610  },  { shardSlot: MaxKey  }, "mldp-cluster-rs5")
```

After this setup, ingestion can start. The first documents are routed to the correct shard immediately.

## Query indexes

Keep query-oriented indexes separate from the shard key.

```javascript
db.buckets.createIndex({ pvName: 1, source: 1 })
db.buckets.createIndex({ pvName: 1, "dataTimestamps.firstTime.seconds": 1, "dataTimestamps.firstTime.nanos": 1, "dataTimestamps.lastTime.seconds": 1, "dataTimestamps.lastTime.nanos": 1 })
db.buckets.createIndex({ shardSlot: 1, "dataTimestamps.firstTime.seconds": 1 })
```

The first index supports normal MLDP PV queries.  
The second supports time-range scans per PV.  
The third supports efficient shard-local scans when `shardSlot` is included in the query.

## Optional query optimization

When querying one PV, the service can look up `shardSlot` from the metadata and include it in the query to avoid scatter-gather:

```javascript
db.buckets.find({
  shardSlot: 42137,
  pvName: "BPMS:DMPH:502:TMITBR",
  "dataTimestamps.firstTime.seconds": { $gte: startSec },
  "dataTimestamps.lastTime.seconds":  { $lte: endSec }
})
```

## Future shard expansion

When the cluster grows from 6 to more shards, split existing slot ranges and move some to the new shards.

Example from 6 to 12 shards:

```text
old: 0 - 10921 on rs0
new:
  0    - 5460  stays on rs0
  5461 - 10921 moves to rs6
```

Repeat for the other original ranges.

No changes to `pvSlotAssignment` documents, no changes to ingestion code. `shardSlot` values are stable forever.

## Validation steps

Before production ingestion:

1. Export the full Gen1 PV list.
2. Run a dry-run of the slot assignment to count how many PVs fall into each shard range.
3. Confirm distribution is acceptable (target: equal ± 5%).
4. Initialize `dp.slotCounters` for each source.
5. Create the empty collection, pre-split, and move ranges (see workflow above).
6. Insert one small Gen1 test event.
7. Run `db.buckets.getShardDistribution()` and verify writes spread across shards.

Useful commands:

```javascript
sh.status()
db.buckets.getShardDistribution()
sh.getBalancerState()
sh.isBalancerRunning()
db.pvSlotAssignment.countDocuments({ source: "gen1_bsas" })
db.slotCounters.aggregate([
  { $match: { source: "gen1_bsas", pvCount: { $gt: 0 } } },
  { $group: { _id: null, min: { $min: "$pvCount" }, max: { $max: "$pvCount" }, total: { $sum: "$pvCount" } } }
])
```

## Success criteria

- First ingestion writes across all 6 shards immediately.
- Same `pvName + source` always lands on the same shard.
- No single shard receives all startup traffic.
- PV distribution is exactly uniform (round-robin, not probabilistic).
- Normal PV/time queries remain efficient.
- Future cluster growth handled by splitting and moving slot ranges only.

## Summary

This design uses reader-managed pre-partitioning with round-robin slot assignment.

Each reader maintains a `pvName → shardSlot` cache backed by MongoDB metadata. New PVs are assigned to the slot with the fewest current PVs for that source, ensuring perfect initial balance. MongoDB is pre-split so those logical slots already map to different shards before the first write.

The result is predictable initial distribution, stable PV locality, zero reliance on hash quality, and easier future scaling.
