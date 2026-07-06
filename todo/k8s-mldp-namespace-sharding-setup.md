# K8s MLDP Namespace — MongoDB Sharding Setup

## Goal

Configure the `mldp` namespace on `k2eg-dev` so the MongoDB sharded cluster is
ready for `shardSlot`-based pre-partitioned ingestion from the Gen1 BSAS reader.

Prerequisites already implemented in driver code:
- `HDF5BsasGen1Reader` emits `shardSlot` as a 5-digit zero-padded string in
  `DataColumn::metadata` (round-robin shard selection + random slot within
  shard range, cached per session).
- Shard boundaries: 6 shards × 10922 slots over `["00000", "65535"]`.

## Cluster topology (k2eg-dev / mldp namespace)

| Resource | Name |
|----------|------|
| Context  | `k2eg-dev` |
| Namespace | `mldp` |
| Mongos service | `mldp-cluster-mongos` (ClusterIP `10.108.11.218:27017`) |
| Mongos pods | `mldp-cluster-mongos-0/1/2` |
| Config server | `mldp-cluster-cfg-0` |
| Shards | `mldp-cluster-rs0` … `mldp-cluster-rs5` |

---

## Steps

### 1. Open a mongosh session on mongos

```bash
kubectl -n mldp exec -it mldp-cluster-mongos-0 -c mongos -- mongosh
```

All subsequent JavaScript runs inside this session.

---

### 2. Verify all 6 shards are registered

```javascript
sh.status()
```

Expected: 6 shards listed as `mldp-cluster-rs0` through `mldp-cluster-rs5`.

If any are missing, add them (replace `<primary-host>` with the StatefulSet DNS):

```javascript
sh.addShard("mldp-cluster-rs0/mldp-cluster-rs0-0.mldp-cluster-rs0.mldp.svc.cluster.local:27017")
// repeat for rs1–rs5
```

---

### 3. Enable sharding on the `dp` database

```javascript
use dp
sh.enableSharding("dp")
```

Idempotent — safe to re-run.

---

### 4. Create the shard key index on `dp.buckets`

```javascript
use dp
db.buckets.createIndex({ shardSlot: 1 })
```

**Must exist before sharding the collection.**

---

### 5. Shard `dp.buckets` on `shardSlot`

```javascript
sh.shardCollection("dp.buckets", { shardSlot: 1 })
```

---

### 6. Pre-split into 6 ranges

```javascript
sh.splitAt("dp.buckets", { shardSlot: "10922" })
sh.splitAt("dp.buckets", { shardSlot: "21844" })
sh.splitAt("dp.buckets", { shardSlot: "32766" })
sh.splitAt("dp.buckets", { shardSlot: "43688" })
sh.splitAt("dp.buckets", { shardSlot: "54610" })
```

> **String not integer.** The driver emits `shardSlot` as a zero-padded
> 5-digit string (`"04711"`, `"17503"`, …). All split points and moveRange
> calls must use quoted strings for correct lexicographic ordering.

---

### 7. Assign each range to a shard

```javascript
sh.moveRange("dp.buckets", { shardSlot: MinKey   }, { shardSlot: "10922" }, "mldp-cluster-rs0")
sh.moveRange("dp.buckets", { shardSlot: "10922"  }, { shardSlot: "21844" }, "mldp-cluster-rs1")
sh.moveRange("dp.buckets", { shardSlot: "21844"  }, { shardSlot: "32766" }, "mldp-cluster-rs2")
sh.moveRange("dp.buckets", { shardSlot: "32766"  }, { shardSlot: "43688" }, "mldp-cluster-rs3")
sh.moveRange("dp.buckets", { shardSlot: "43688"  }, { shardSlot: "54610" }, "mldp-cluster-rs4")
sh.moveRange("dp.buckets", { shardSlot: "54610"  }, { shardSlot: MaxKey  }, "mldp-cluster-rs5")
```

`moveRange` is synchronous — wait for each command to return before the next.

---

### 8. Disable the balancer for `dp.buckets`

After manual pre-split, prevent automatic rebalancing:

```javascript
sh.disableBalancing("dp.buckets")
```

Re-enable only when intentionally expanding the cluster.

---

### 9. Create supporting indexes

```javascript
use dp

db.buckets.createIndex({ pvName: 1, source: 1 })
db.buckets.createIndex({
  pvName: 1,
  "dataTimestamps.firstTime.seconds": 1,
  "dataTimestamps.lastTime.seconds": 1
})
db.buckets.createIndex({
  shardSlot: 1,
  "dataTimestamps.firstTime.seconds": 1
})
```

---

### 10. Validate before ingestion

```javascript
sh.status()
db.buckets.getShardDistribution()
sh.getBalancerState()
```

Insert a test document and confirm it lands on the correct shard:

```javascript
db.buckets.insertOne({ pvName: "TEST:PV", source: "gen1_bsas", shardSlot: "04711" })
db.buckets.find({ shardSlot: "04711" }).explain("executionStats")
// shards.mldp-cluster-rs0 should show nReturned: 1
```

Cleanup after test:
```javascript
db.buckets.deleteOne({ pvName: "TEST:PV" })
```

---

## Slot ranges reference

| Shard              | Min (inclusive) | Max (exclusive) | DNS |
|--------------------|----------------|----------------|-----|
| `mldp-cluster-rs0` | `"00000"`       | `"10922"`       | `mldp-cluster-rs0.mldp.svc.cluster.local` |
| `mldp-cluster-rs1` | `"10922"`       | `"21844"`       | `mldp-cluster-rs1.mldp.svc.cluster.local` |
| `mldp-cluster-rs2` | `"21844"`       | `"32766"`       | `mldp-cluster-rs2.mldp.svc.cluster.local` |
| `mldp-cluster-rs3` | `"32766"`       | `"43688"`       | `mldp-cluster-rs3.mldp.svc.cluster.local` |
| `mldp-cluster-rs4` | `"43688"`       | `"54610"`       | `mldp-cluster-rs4.mldp.svc.cluster.local` |
| `mldp-cluster-rs5` | `"54610"`       | `"65536"`       | `mldp-cluster-rs5.mldp.svc.cluster.local` |

All 65,536 slots covered. Driver assigns slots round-robin across shards with
random placement within each shard's range — expected ~217 PVs per shard for a
1300-column Gen1 file.

## Related documents

- `todo/MLDP_Bucket_PrePartitioning_Logic.md` — full design rationale, slot
  assignment algorithm, future scaling plan
