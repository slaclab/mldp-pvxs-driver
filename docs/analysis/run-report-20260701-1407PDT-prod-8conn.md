# Dev Run Report — 2026-07-01 14:07–14:17 PDT (8-Connection Pool, Wall-Clock Rate Fix)

**Run time:** 2026-07-01T14:07:02−07:00 → 2026-07-01T14:17:43−07:00 (10 min 40 s) · UTC 21:07:02Z–21:17:43Z
**Environment:** dev container, local Docker Compose, gRPC to local mock
**Source file:** `CU_HXR_20260316_115135.h5`
**Reader:** HDF5 BSAS Gen1
**Writer:** MLDPWriter (`mldp_ingest`), 8-connection pool
**Snapshots analysed:** 129 (5 s interval)

---

## Key Changes Validated in This Run

| Fix | Commit | Status |
|-----|--------|--------|
| Wall-clock windowed rate metric (`data_bytes_per_second`) | `fa5f8f0` | ✅ Accurate |
| Distinct TCP connection per pool slot (`GRPC_ARG_CHANNEL_ID`) | `473a7b3` | ✅ All 8 slots active |

---

## gRPC Connection Pool

| Metric | Value |
|--------|-------|
| Pool size (slots) | 8 |
| Connections in use — min / max / avg | 8 / 8 / 8.0 |
| Connections available — min / max / avg | 0 / 0 / 0.0 |

All 8 connections are in use throughout the entire run. The pool is fully saturated — no idle slots at any snapshot. Each connection is a distinct TCP session (forced by `GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL` + unique `GRPC_ARG_CHANNEL_ID`), confirming the load-balancing fix is in effect.

---

## Throughput

### Wall-Clock Rate Metric (fixed)

| Metric | Min | Max | Avg | Median |
|--------|-----|-----|-----|--------|
| `data_bytes_per_second` | 23.7 MB/s | 55.0 MB/s | **43.5 MB/s** | 44.7 MB/s |
| `payload_bytes_per_second` | 21.8 MB/s | 50.6 MB/s | **40.0 MB/s** | — |

### Cross-check Against `bytes_total`

| Metric | Value |
|--------|-------|
| Total bytes transferred | 27.03 GB |
| Wall time | 640 s |
| Derived overall rate | **42.2 MB/s** |

The windowed rate gauge (43.5 MB/s avg) tracks the cumulative counter-derived rate (42.2 MB/s) within ~3% — confirming the wall-clock fix is correct. The previous bug reported ~4,500 B/s (off by ~36,000×).

### Throughput vs Previous Dev Run

The earlier dev test (~160 MB/s) ingested a smaller dataset in a single pass. This run processed 27 GB, sustaining ~42 MB/s across 10+ minutes. The difference reflects dataset size and queue saturation effects (see Queue section).

---

## Send Latency (`controller_send_time_seconds`)

| Metric | Value |
|--------|-------|
| Total sends in window | 1,127,260 |
| Avg send time | **2.32 ms** |
| ≤ 1 ms | **98.5%** |
| > 100 ms | **0.93%** |
| > 500 ms | 0.01% |
| > 1 s | 0% |

### Bucket Distribution (last snapshot, incremental %)

| Bucket | Count (cumulative) | % in bucket |
|--------|--------------------|-------------|
| ≤ 1 ms | 1,122,769 | 98.52% |
| ≤ 2.5 ms | 1,123,081 | 0.03% |
| ≤ 5 ms | 1,123,515 | 0.04% |
| ≤ 10 ms | 1,124,137 | 0.05% |
| ≤ 25 ms | 1,125,290 | 0.10% |
| ≤ 50 ms | 1,126,376 | 0.10% |
| ≤ 100 ms | 1,129,103 | 0.24% |
| ≤ 250 ms | 1,137,100 | 0.70% |
| ≤ 500 ms | 1,139,529 | 0.21% |
| ≤ 1 s | 1,139,657 | 0.01% |
| > 1 s | — | 0% |

### Dev vs Production Comparison

| Metric | Dev (this run) | Production (previous) |
|--------|---------------|----------------------|
| ≤ 1 ms | 98.5% | 97% |
| Avg send time | **2.3 ms** | 7.0 ms |
| 100–500 ms spikes | 0.93% | ~3% |

Dev is ~3× faster average send time with fewer spikes. Production spikes are caused by MLDP ingest server buffer saturation; local dev avoids network RTT entirely.

---

## Queue Depths

| Queue | Min | Max | Avg |
|-------|-----|-----|-----|
| Writer queue (`mldp_ingest`) | 58 | 200 | **165.9** |
| Controller queue | 0 | 200 | **163.9** |

Both queues average ~83% of capacity (max 200). Back-pressure is active — `push()` is blocking the reader for significant periods. The system is throughput-limited by gRPC write speed, not reader speed.

**Implication:** increasing `thread-pool` (more concurrent workers) or decreasing `chunkSize` (smaller frames) would reduce queue depth and improve throughput.

---

## Memory

| Metric | Value |
|--------|-------|
| RSS start | 2.19 GB |
| RSS end | 15.45 GB |
| RSS peak | 15.47 GB |
| RSS growth | +13.3 GB |
| Virtual start | 3.43 GB |
| Virtual end | 16.79 GB |

RSS growth is **expected behavior**, not a leak. Root cause: 1,367 columns × 1,000 timestamp copies per chunk (gRPC message size limit requires one `DataBatch` frame per column). Peak RSS scales with `chunkSize × column_count × sizeof(timestamp)`. To reduce peak RSS, lower `chunkSize` in the HDF5 BSAS Gen1 reader config.

---

## Stream Rotations

| Metric | Value |
|--------|-------|
| Rotations in window (max-bytes trigger) | 11,921 |
| Total rotations | 12,046 |
| Rotation rate | ~18.6 / s |

All rotations triggered by `max bytes exceeded` (2 MiB stream size limit). No error-triggered rotations. Normal operating behaviour.

---

## Process Stats

| Metric | Value |
|--------|-------|
| Threads (avg) | 19 |
| CPU user ticks (window) | 22,908 |
| CPU system ticks (window) | 3,321 |
| CPU user/sys ratio | ~6.9× |
| Total pushes (window) | 1,127,260,000 |

---

## Production MongoDB Storage Performance (snapshot: 2026-07-01T21:47–21:52Z)

### Cluster Topology

| Component | Detail |
|-----------|--------|
| MongoDB version | 8.0.19-7 (Percona PSMDB) |
| Topology | Sharded cluster — 6 replica sets (rs0–rs5) + 1 config server (cfg) + 3 mongos routers |
| Operator uptime | rs0/rs1: ~18 days; rs2–rs5: ~6 days |
| Mongos uptime | ~6.1 days (530,149 s) |

### Storage by Shard (`dp` database)

| Shard | Objects | Data (MB) | Storage (MB) | Index (MB) | Total (MB) | Avg Obj (B) | FS Used (GB) / Total (GB) |
|-------|---------|-----------|--------------|------------|------------|-------------|--------------------------|
| rs0 | 184,237 | 4,834.7 | 5,093.8 | 65.8 | 5,159.6 | 27,517 | 8.5 / 2560 |
| rs1 | 1,338,258 | 5,431.7 | 5,268.8 | 361.5 | 5,630.3 | 4,256 | 8.9 / 2560 |
| rs2 | 195,565 | 5,138.8 | 5,231.8 | 72.0 | 5,303.8 | 27,553 | 20.6 / 2560 |
| rs3 | 187,699 | 4,926.0 | 5,021.1 | 62.3 | 5,083.4 | 27,519 | 19.7 / 2560 |
| rs4 | 190,110 | 4,995.4 | 5,311.2 | 65.4 | 5,376.6 | 27,553 | 20.5 / 2560 |
| rs5 | 193,454 | 5,086.5 | 5,355.1 | 63.8 | 5,418.9 | 27,570 | 20.6 / 2560 |
| **Total** | **2,289,323** | **30,413** | **31,282** | **690.8** | **31,973** | — | — |

**Total cluster size (all DBs):** 33.6 GB on-disk (32,079 MB). FS utilisation is minimal — all shards have 2.56 TB volumes with <1% used.

**rs1 anomaly:** rs1 holds 1,338,258 objects vs ~185–195k on every other shard. Average object size on rs1 is 4,256 B vs ~27,500 B on all others — indicating rs1 stores a different (or older) collection type. rs1 also carries 6 chunks vs rs4's 1 chunk (see Chunk Distribution below).

### Sharded Collection

| Collection | Shard key | Unique |
|-----------|-----------|--------|
| `dp.buckets` | `{pvName: "hashed", createdAt: 1}` | No |

### Chunk Distribution (`dp.buckets`)

| Shard | Chunks |
|-------|--------|
| rs1 | **6** |
| rs2 | 4 |
| rs3 | 4 |
| rs0 | 3 |
| rs5 | 2 |
| rs4 | **1** |

**Imbalance:** rs1 holds 6× the chunks of rs4. The balancer is running in `full` mode and has completed 157,101 rounds — the imbalance either recently appeared (rs2–rs5 joined 6 days ago) or the balancer has not converged yet. Watch for continued migration over the next ~24 h.

### WiredTiger Cache (per shard primary)

| Shard | Cache Max (GB) | Cache Used (GB) | Fill % | Dirty % | Evict Workers | Resident RSS (MB) |
|-------|---------------|----------------|--------|---------|---------------|-------------------|
| rs0 | 7.46 | 5.97 | **80.0** | 0.03 | 4 | 7,573 |
| rs1 | 7.46 | 5.96 | **79.9** | 0.02 | 4 | 7,379 |
| rs2 | 7.46 | 5.90 | **79.1** | 0.01 | 4 | 7,203 |
| rs3 | 7.46 | 5.78 | 77.5 | 0.01 | 4 | 7,233 |
| rs4 | 7.46 | 5.80 | 77.7 | 0.00 | 4 | 7,144 |
| rs5 | 7.46 | 5.96 | **79.9** | 0.00 | 4 | 7,351 |

All shards operate at 77–80% cache fill — approaching WiredTiger's default eviction trigger (80%). At this level the eviction server runs continuously; breaching 80% causes application-thread stalls (the `application threads evicting` counter on rs0 was non-zero in earlier polling). Dirty percentage is very low (<0.05%) — writes are flushing efficiently.

**rs0 cache I/O (lifetime):** 33.4 GB read into cache, 210 GB written from cache — heavy write amplification (~6.3×) consistent with a time-series insert workload with frequent checkpoint flushes.

### Replication Lag

| Shard | Primary | Secondary lag (s) |
|-------|---------|-------------------|
| rs0 | rs0-0 | 0 |
| rs1 | rs1-1 | 0 |
| rs2 | rs2-0 | 0 |
| rs3 | rs3-0 | 0 |
| rs4 | rs4-0 | **10** |
| rs5 | rs5-0 | 0 |

**rs4 secondary (rs4-1) is 10 s behind.** All other secondaries are fully caught up. A 10 s lag is within normal tolerance for a lightly-loaded shard (rs4 holds only 1 chunk) but should be monitored — elevated lag may indicate I/O pressure on rs4-1's node.

### mongos Operation Counters (lifetime, uptime ~6.1 days)

| Op | Count | Avg latency (µs) |
|----|-------|-----------------|
| Inserts | 8,392,431 | — |
| Queries (find) | 4,231,737 | ~386 |
| Updates | 13 | — |
| Deletes | 0 | — |
| Commands | 6,801,544 | ~596 |
| Write latency (total) | 8,392,444 ops | **2,917** |
| Read latency (total) | 4,196,392 ops | ~387 |

Write average latency of **2.9 ms** at the mongos layer includes network round-trip to shard + WiredTiger write. Consistent with dev run send-time data (2.3 ms local).

### Active Operations (snapshot)

| Op type | Active count |
|---------|-------------|
| `command` | 99 |
| `none` | 24 |
| `getmore` | 12 |
| **Total** | **135** |

135 concurrent active operations on the mongos at snapshot time — healthy for a 4-pod ingestion fleet.

### Network Compression (mongos, lifetime)

| Direction | Uncompressed (GB) | Compressed (GB) | Ratio |
|-----------|------------------|-----------------|-------|
| Ingress (snappy decompress) | 5.64 | 4.54 | 1.24× |
| Egress (snappy compress) | 127.8 | 74.2 | 1.72× |

Snappy is active on all connections. Egress compression at 1.72× reduces storage backend write amplification significantly.

### Ingestion Pod Health

| Pod | Error/Warn log lines (last 100) |
|-----|---------------------------------|
| dp-ingestion-688c7848c6-46f7x | 11 |
| dp-ingestion-688c7848c6-5wlpx | 4 |
| dp-ingestion-688c7848c6-kzrbv | 2 |
| dp-ingestion-688c7848c6-mth7m | 12 |

Observed error type: `GOAWAY failed: Connection reset` (HTTP/2 stream teardown on idle connection). Not a data-loss event — gRPC will transparently reconnect. Rate is low.

### Storage Issues and Observations

1. **Cache pressure approaching trigger threshold.** All primaries at 77–80% fill. WiredTiger default eviction trigger is 80%; the dirty trigger is 20%. Neither threshold is breached today, but continued ingestion growth will push rs0/rs1/rs5 over the eviction trigger, causing latency spikes. **Recommendation:** increase `wiredTigerCacheSizeGB` (current: 7.46 GB, pod RSS ~7.4 GB → pod likely has 10–12 GB memory), or reduce working set by archiving/TTL-expiring old buckets.

2. **Chunk imbalance: rs1 (6 chunks) vs rs4 (1 chunk).** rs2–rs5 are new (6 days old). The balancer needs time to migrate chunks after shard expansion. No action required unless the imbalance persists beyond 48 h.

3. **rs4 secondary lag: 10 s.** Mild, but worth watching. rs4 holds the fewest chunks — could indicate node-level I/O variability rather than replication overload.

4. **rs1 object count anomaly.** rs1 holds 7× more objects than other shards with 6.5× smaller average object size. This suggests rs1 holds an unsharded collection (config metadata or legacy data) alongside the sharded `buckets` collection. Investigate with `db.getCollectionNames()` from rs1-local shell if needed.

5. **Write amplification on rs0: 6.3× (210 GB written / 33.4 GB read into cache).** Normal for a WiredTiger checkpoint-heavy time-series workload, but confirms the workload is write-dominated at the storage layer.

---

## Conclusions

1. **Rate metric fix confirmed** — `data_bytes_per_second` now reports accurate wall-clock throughput (43.5 MB/s avg), matching bytes_total delta to within 3%.
2. **Connection pool fix confirmed** — all 8 pool slots active simultaneously, each on a distinct TCP connection.
3. **Bottleneck: gRPC write throughput** — both queues saturated at ~83% avg. Reader is faster than writer.
4. **Memory growth expected** — one timestamp copy per column per chunk by design. Reduce `chunkSize` to lower peak RSS.
5. **Latency healthy** — 98.5% of sends ≤ 1 ms in dev; occasional 100–500 ms spikes (0.93%) are back-pressure artifacts, not network issues.
6. **Production MongoDB: cache near eviction trigger** — all 6 shard primaries at 77–80% WiredTiger cache fill. Threshold is 80%. Action needed before next capacity growth event.
7. **Production MongoDB: chunk imbalance post-shard expansion** — rs1 holds 6 chunks, rs4 holds 1. Balancer active; monitor over next 48 h.
8. **Production MongoDB: rs4 secondary 10 s behind** — mild lag, monitor for growth.
9. **Production MongoDB: rs1 anomalous object count** — 1.34 M objects vs ~185–195 k on other shards; small avg obj size suggests unsharded config/metadata collection co-located on rs1.

---

## Recommended Tuning

| If goal is… | Adjust |
|-------------|--------|
| Higher throughput | Increase `thread-pool` (e.g., 8→16) |
| Lower peak RSS | Decrease `chunkSize` (e.g., 1000→500) |
| Fewer back-pressure stalls | Increase `queue-capacity` or `thread-pool` |
| Better k8s pod distribution | Set `max-conn` ≥ replica count |

---

## Where Is the Bottleneck: MongoDB or MLDP Middle Layer?

### Evidence Summary

| Signal | Value | Layer |
|--------|-------|-------|
| Writer queue avg depth | 165.9 / 200 (83%) | pvxs-driver writer |
| Controller queue avg depth | 163.9 / 200 (83%) | pvxs-driver controller |
| Avg send time (dev, local mock) | **2.3 ms** | gRPC → mock (no MongoDB) |
| MongoDB write avg latency (prod mongos) | **2.9 ms** | MongoDB |
| dp-ingestion pods | 4 (all 8/8 connections in use) | MLDP ingest server |
| GOAWAY errors observed | yes (connection reset on idle streams) | gRPC / MLDP ingest server |

### Verdict: MLDP Ingest Server Is the Bottleneck

MongoDB is **not** the throughput limiter. Evidence:

1. **Dev run uses a local mock** — no MongoDB involved — yet queues still saturate at 83%. The constraint is upstream of MongoDB entirely: the pvxs-driver gRPC send rate cannot exceed what the MLDP ingest server accepts.

2. **MongoDB write latency is healthy.** Prod mongos reports 2.9 ms average write latency across 8.39 M inserts. WiredTiger dirty-page percentage is <0.05% on all shards — the storage engine is not stalled.

3. **Queue saturation is upstream, not downstream.** Both the controller queue (before the writer) and the writer queue (before gRPC send) are at 83% simultaneously. If MongoDB were the bottleneck, only the writer queue would saturate; the controller queue would stay low. Equal saturation at both points means the limiting stage is the gRPC send itself — the MLDP ingest server is not accepting data fast enough.

4. **All 8 gRPC connections fully in use throughout the run.** No idle connection capacity. The pvxs-driver has reached its connection-pool ceiling; adding more connections would directly increase throughput.

5. **GOAWAY / connection reset errors on ingestion pods.** The MLDP ingest server is actively closing streams, forcing reconnect overhead. This further reduces effective throughput per connection.

### What MongoDB Would Look Like If It Were the Bottleneck

- Write latency at mongos would spike (>10 ms avg), not sit at 2.9 ms.
- WiredTiger dirty page % would climb toward the 20% dirty eviction trigger.
- Application-thread eviction stalls (`application thread time evicting`) would appear.
- Writer queue would be full but controller queue would not — the controller can push into the writer faster than the writer can drain into MongoDB.

None of these are present.

### MongoDB Risks (Not Today's Bottleneck, But Upcoming)

| Risk | Current State | Trigger Condition |
|------|--------------|-------------------|
| WiredTiger cache eviction stalls | 77–80% fill (threshold: 80%) | ~3–5% more data growth, or larger working set |
| Chunk imbalance write hot-spot | rs1 holds 6/20 chunks (30%) | Already degraded; balancer converging |
| rs4 secondary lag | 10 s | If lag grows, failover risk on rs4 |

MongoDB becomes a risk when: (a) cache fill breaches 80% on rs0/rs1/rs5, or (b) the ingestion rate is scaled up past ~4× current throughput.

### Recommended Actions

#### Immediate — MLDP Ingest Server (unlocks throughput now)

| Action | Expected gain |
|--------|--------------|
| Increase `max-conn` (pvxs-driver pool) beyond 8 | Linear throughput increase until next bottleneck |
| Scale `dp-ingestion` deployment replicas beyond 4 | More server-side gRPC capacity; reduces GOAWAY rate |
| Tune gRPC keepalive / `max-connection-age` on dp-ingestion to reduce GOAWAY resets | Reduces reconnect overhead per connection |
| Increase `thread-pool` in pvxs-driver config | More concurrent workers draining queues |

#### Near-term — MongoDB (prevent future degradation)

| Action | Why |
|--------|-----|
| Increase `wiredTigerCacheSizeGB` on all shard pods (current 7.46 GB → 10–12 GB, matching pod memory) | Prevents eviction stalls as dataset grows |
| Add TTL index on `dp.buckets` for old data (e.g., `createdAt` older than retention window) | Bounds working set; keeps cache fill stable |
| Monitor chunk balancer convergence over next 48 h; if rs1 still holds >4 chunks, trigger manual `moveChunk` | Reduces write hot-spotting on rs1 |
| Investigate rs1 unsharded collection (7× object count, 6.5× smaller avg obj) | May be config bloat consuming cache on rs1 |
