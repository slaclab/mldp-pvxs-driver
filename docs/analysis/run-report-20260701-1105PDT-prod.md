# Run Report — 2026-07-01 11:05–11:32 PDT (Production) · UTC 18:05–18:32Z

**Duration:** 27 min  
**Source file:** `CU_HXR_20260316_115135.h5`  
**Controller:** `default`  
**Writer:** `mldp_ingest`

mongodb colleciton at the end of the run:
* dataSize: 31.54 GB (29.37 GiB)
* storageSize: 18.25 GB (17.00 GiB)
---

## Summary

Single HDF5 replay run. Reader burst-loaded data at ~72 MB/s for ~7 min, saturating the controller queue and driving RSS to 35 GB peak before dropping back to ~9 GB after the burst. Writer queue was saturated (depth ≥ 199) for 99% of the run. System drained cleanly — both queues at 0 by end.

---

## Timeline

| Time (UTC) | Event |
|---|---|
| 18:05:14 | Run start. Controller queue already at 610, writer queue at 199 (saturated). |
| 18:06:29 | Reader active — burst at **72 MB/s**. RSS begins rapid growth. |
| 18:06:34 | **RSS peak: 35.17 GB** (virt peak: 36.59 GB). |
| 18:13:51 | Reader burst ends. RSS drops to 9.14 GB (likely stream rotation + GC). |
| 18:13:51–18:30 | Reader idle. Queues draining. Memory climbs slowly (~1 GB/8 min). |
| 18:30:01 | Reader resumes at ~9.2 MB/s (tail or second pass). |
| 18:32:16 | Run end. Controller queue = 0, writer queue = 0. |

---

## Data Throughput

| Metric | Value |
|---|---|
| Reader events (unique HDF5 events) | 838 |
| Reader events received (total rows) | 837,175 |
| Reader processing time | 986,937 ms (~16.4 min active) |
| Reader peak throughput | 72.0 MB/s |
| Reader avg throughput (when active) | ~24 MB/s |
| Writer data bytes total | **27.44 GB** |
| Writer payload bytes total | 25.26 GB |
| Writer avg throughput | ~4,490 B/s (~4.4 KB/s) |
| Writer push total | 1,144,418,225 |

> Writer throughput is low (~KB/s vs reader ~MB/s) — reader reads raw HDF5 bulk; writer sends processed/filtered payload to MLDP ingest.

---

## Queue Behavior

| Metric | Min | Max | Avg |
|---|---|---|---|
| Controller queue depth | 0 | 812 | 214 |
| Writer queue depth | 0 | 200 | 198 |

Writer queue was at capacity (≥199) for **99% of run** — writer was the bottleneck throughout. Controller queue hit 812 during the reader burst, then drained as reader went idle.

---

## Send Time (controller → writer)

Measured over 1,145,546 sends, total 7,434 s.

| Stat | Value |
|---|---|
| Mean | 6.49 ms |
| p50 | ≤ 1 ms |
| p90 | ≤ 1 ms |
| p99 | ≤ 500 ms |

The long tail (p99 ≤ 500 ms) is consistent with writer queue backpressure causing occasional blocking sends.

---

## Memory

| Metric | Value |
|---|---|
| RSS at start | 14.78 GB |
| RSS peak | **35.17 GB** at 18:06:34 |
| RSS after burst drop | 9.14 GB |
| RSS at end | 15.93 GB |
| Virtual peak | 36.59 GB |

Large transient spike (~+20 GB in ~5 s) during reader burst suggests buffering of HDF5 read-ahead or event batch accumulation. Drop is sharp — consistent with a stream rotation triggering buffer release.

---

## Stream Rotations

| Metric | Value |
|---|---|
| Rotations this run | 11,584 |
| Rotation reason | `max bytes exceeded` |
| Total at run end | 12,096 |

High rotation count is expected given the volume (27 GB) — each rotation triggered when a stream segment hit its byte limit.

---

## Process Stats (final sample)

| Metric | Value |
|---|---|
| Threads | 17 |
| Open FDs | 7 |
| CPU user ticks | 22,908 |
| CPU sys ticks | 3,517 |
| Voluntary context switches | 11,623 |
| Pool connections (ingestion, in-use) | 8 |
| Pool connections (ingestion, available) | 0 |

All 8 ingestion pool connections in use throughout — pool fully saturated, no headroom.

---

## Observations

1. **Writer is the throughput bottleneck.** Queue at max depth for 99% of run. MLDP ingest connection pool (8/8) fully occupied.
2. **Memory spike is transient but large.** 35 GB peak from a 72 MB/s burst lasting ~7 min. Worth investigating if this is unbounded buffering or expected HDF5 read-ahead.
3. **Clean drain.** Both queues reach 0 by run end — no data loss signal.
4. **Send latency is bimodal.** p90 ≤ 1 ms but p99 ≤ 500 ms — occasional long waits match backpressure episodes.

---

## Where Is the Bottleneck: MongoDB or MLDP Middle Layer?

### Evidence Summary

| Signal | Value | Layer |
|--------|-------|-------|
| Writer queue at max depth | 99% of run | pvxs-driver writer |
| Reader peak throughput | 72 MB/s (burst) | HDF5 reader |
| Reader avg throughput (active) | ~24 MB/s | HDF5 reader |
| Effective writer throughput | ~KB/s vs reader ~MB/s | gRPC → MLDP ingest |
| Connection pool (ingestion) | 8/8 in use, 0 available | MLDP ingest server |
| MongoDB write avg latency (prod mongos) | **2.9 ms** | MongoDB |
| MongoDB WiredTiger dirty % | <0.05% all shards | MongoDB |

### Verdict: MLDP Ingest Server Is the Bottleneck, Not MongoDB

1. **Reader burst rate (72 MB/s) far exceeds writer drain rate.** The gap between reader throughput and effective writer throughput is the defining observation. MongoDB write latency at 2.9 ms would support roughly 280 ops/s per connection × 8 connections = ~2,240 ops/s — the MLDP ingest server's gRPC accept rate, not MongoDB's storage speed, limits how fast those ops arrive.

2. **Both queues saturate simultaneously.** Controller queue (before the writer) and writer queue (before gRPC send) both hit their ceiling. If MongoDB were slow, only the writer queue would fill; the controller would stay clear. Equal saturation means the gRPC send layer is the constraint.

3. **Writer throughput reported as KB/s while reader runs at MB/s.** The ~1,000× throughput gap is the MLDP ingest server's batch-acceptance rate, not a storage write speed. MongoDB writing ~2.9 ms per op is fast; the server is not accepting batches fast enough.

4. **All 8 pool connections occupied with 0 available throughout.** Connection pool ceiling is reached. More connections would allow more concurrent gRPC streams and directly raise throughput.

5. **MongoDB state is healthy.** WiredTiger cache 77–80% fill (below the 80% eviction trigger), dirty pages <0.05%, zero application-thread eviction stalls, replication lag 0 on 5/6 shards. None of the MongoDB degradation signatures are present.

### What MongoDB Would Look Like If It Were the Bottleneck

- mongos write latency would spike beyond 10–20 ms average.
- WiredTiger dirty page % would climb toward 20% (dirty eviction trigger).
- `application thread time evicting` counter would be non-zero and growing.
- Writer queue would be full; controller queue would not — the controller could push into the writer faster than MongoDB drained it.

None of these conditions are present.

### MongoDB Risks (Not Today's Bottleneck, But Upcoming)

| Risk | Current State | Trigger Condition |
|------|--------------|-------------------|
| WiredTiger cache eviction stalls | 77–80% fill (threshold: 80%) | Continued ingestion growth or larger working set |
| Chunk imbalance write hot-spot | rs1: 6/20 chunks (30%); rs4: 1/20 | Balancer converging post-shard-expansion; monitor 48 h |
| rs4 secondary lag | 10 s | If lag grows → failover risk on rs4 |

### Recommended Actions

#### Immediate — MLDP Ingest Server (unlocks throughput now)

| Action | Expected gain |
|--------|--------------|
| Scale `dp-ingestion` replicas beyond 4 | More server-side gRPC capacity |
| Increase `max-conn` in pvxs-driver pool (8 → 16+) | Linear throughput increase until next bottleneck |
| Tune gRPC `max-connection-age` / keepalive on dp-ingestion | Reduces GOAWAY / reconnect overhead |
| Increase pvxs-driver `thread-pool` | More concurrent workers draining the controller queue |

#### Near-term — MongoDB (prevent future degradation)

| Action | Why |
|--------|-----|
| Increase `wiredTigerCacheSizeGB` (7.46 GB → 10–12 GB per pod) | Prevents eviction stalls as 31 GB working set keeps growing |
| Add TTL index on `dp.buckets.createdAt` | Bounds working set; stabilises cache fill long-term |
| Monitor chunk balancer over next 48 h; manually `moveChunk` if rs1 still holds >4 chunks | Removes write hot-spot on rs1 |
| Investigate rs1 unsharded collection (1.34 M objects, 4.3 KB avg vs 27.5 KB elsewhere) | May consume rs1 cache and index memory unnecessarily |
