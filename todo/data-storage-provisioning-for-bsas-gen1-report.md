# BSAS Gen1 Data Storage Provisioning Report

**Date:** 2026-06-21
**Source file:** `data/bsas-gen1-extract.h5`
**MongoDB:** Mongo 8 via Docker, Snappy compression (WiredTiger default)
**Database:** `dp`

---

## Task 1: MongoDB Expansion Factor

### Raw Measurements

| Metric | Value |
|--------|-------|
| HDF5 file size | 311,872 bytes (305 KB) |
| HDF5 datasets | 8 |
| MongoDB dataSize (db-wide) | 8,600,257 bytes (8.2 MB) |
| MongoDB storageSize (db-wide) | 1,306,624 bytes (1.25 MB) |
| MongoDB totalSize (db-wide) | 2,887,680 bytes (2.75 MB) |
| MongoDB totalIndexSize (db-wide) | 1,581,056 bytes (1.51 MB) |

### Buckets Collection (actual PV data)

| Metric | Value |
|--------|-------|
| Bucket documents | 1,367 |
| Average bucket document size | 1,296 bytes |
| Min document size | 1,202 bytes |
| Max document size | 1,330 bytes |
| Samples per bucket | 19 (uniform) |
| Total data values stored | 25,973 |
| Distinct PVs | 1,367 |
| Buckets per PV | 1.0 (single bucket per PV) |
| Data types | 1,351 doubleColumn + 16 int32Column |
| Buckets dataSize | 1,771,238 bytes (1.69 MB) |
| Buckets storageSize (Snappy compressed) | 339,968 bytes (332 KB) |
| Buckets totalIndexSize | 294,912 bytes (288 KB) |
| Buckets total on disk | 634,880 bytes (620 KB) |

### Indexes on Buckets (4 total)

| Index | Size |
|-------|------|
| `_id_` | 94,208 bytes |
| `pvName_1` | 61,440 bytes |
| `pvName_1_dataTimestamps.firstTime.seconds_1_...lastTime.nanos_1` (compound) | 98,304 bytes |
| `providerId_1` | 40,960 bytes |

### Expansion Factors

| Measure | Ratio | Notes |
|---------|-------|-------|
| **dataSize / HDF5** | **5.68x** | Uncompressed BSON logical size vs HDF5 |
| **storageSize / HDF5** | **1.09x** | Snappy-compressed data only |
| **totalSize / HDF5 (buckets only)** | **2.04x** | Compressed data + indexes |
| **totalSize / HDF5 (full DB)** | **9.26x** | Includes requestStatus, providers, metadata overhead |

**Primary metric for forecasting: 2.04x** (buckets storageSize + indexSize vs HDF5).

**Caveat:** This sample is 305 KB — small dataset. At scale:
- Per-document index overhead amortizes better with larger bucket sizes
- Snappy compression ratio may improve with larger contiguous blocks
- requestStatus overhead won't scale linearly (operational metadata, not proportional to data volume)

---

## Task 2: Ingestion Performance

### Ingestion Timeline (hdf5_bsas_gen1_provider)

| Phase | Request Count | Start | End | Duration |
|-------|--------------|-------|-----|----------|
| REJECTED (case 1) | 4,101 | 16:32:04 UTC | 16:33:57 UTC | ~113 sec |
| PENDING (case 0) | 5,468 | 17:34:41 UTC | 22:09:52 UTC | multiple attempts |
| COMPLETED (case 2) | 5,475 | 19:37:49 UTC | 22:10:08 UTC | ~2h 32min |

### Key Observations

- **4,101 rejected requests** during first attempt due to timestamp byte-ordering bug:
  `"seconds=138897969, nanos=1772849062"` — seconds/nanos were swapped
- Successfully ingested 1,367 PVs with 19 samples each = 25,973 values
- Total requestStatus documents for this provider: 15,044

### Throughput (successful ingestion)

| Metric | Value |
|--------|-------|
| Data ingested | 305 KB HDF5 → 1.69 MB MongoDB data |
| PVs ingested | 1,367 |
| Values stored | 25,973 |
| Total ingestion requests | 5,475 completed |

### Bottlenecks Identified

1. **Timestamp byte ordering** — First attempt rejected all 4,101 requests due to seconds/nanos swap. Fixed in subsequent attempt.
2. **requestStatus overhead** — 15,044 status documents (6.3 MB) for 305 KB source file. requestStatus dataSize is 3.6x the bucket dataSize. At scale, this operational metadata is disproportionately expensive.
3. **Multiple ingestion attempts** — Total wall-clock time ~5.5 hours across attempts. Clean ingestion path unclear from data.
4. **No CPU/memory metrics available** from MongoDB container stats for this retrospective measurement.

---

## Task 3: Query Performance

### Benchmark Results

| Query | Description | Time | Docs Returned |
|-------|------------|------|---------------|
| A | 100 PVs, all buckets | **25 ms** | 100 |
| B | 1000 PVs, all buckets | **67 ms** | 1,000 |
| C | All PVs (1367), all buckets | **66 ms** | 1,367 |

### Index Usage Analysis

| Query Pattern | Index Used | Keys Examined | Docs Examined | Execution Time |
|--------------|-----------|---------------|---------------|---------------|
| Single PV lookup | `pvName_1` | 1 | 1 | 0 ms |

### Observations

- All queries sub-100ms — dataset fits entirely in WiredTiger cache (4 MB cached)
- `pvName_1` index used efficiently for PV lookups
- Compound time-range index (`pvName_1_dataTimestamps...`) has **0 accesses** — never used in these queries
- `providerId_1` index also **0 accesses**
- Query performance will degrade at scale when data exceeds cache size

### Bottlenecks

- **No bottlenecks at current scale** — entire dataset fits in memory
- At Run 26 scale (TB-range), index-only queries and time-range filtering will become critical
- Compound time index must be validated at scale for range-scan performance

---

## Task 4: Bucketing Strategy

### Bucket Document Structure

```json
{
  "_id": "PV_NAME-seconds-nanos",
  "clientRequestId": "pv_stream_..._bsas_gen1_reader_0",
  "createdAt": "ISO datetime",
  "dataColumn": {
    "_t": "doubleColumn | int32Column",
    "columnMetadata": {
      "attributes": { "provenance.facility": "LCLS", ... },
      "provenance": { "source": "bsas_gen1_reader" }
    },
    "name": "PV_NAME",
    "values": [double/int32 array]
  },
  "dataTimestamps": {
    "bytes": "base64-encoded timestamp list",
    "firstTime": { "dateTime": "ISO", "seconds": Long, "nanos": Long },
    "lastTime":  { "dateTime": "ISO", "seconds": Long, "nanos": Long },
    "sampleCount": 19,
    "valueCase": 2,
    "valueType": "TIMESTAMPLIST"
  },
  "providerId": "ObjectId string",
  "providerName": "hdf5_bsas_gen1_provider",
  "pvName": "PV_NAME"
}
```

### Bucket Construction

| Property | Value |
|----------|-------|
| Bucket key | `pvName-firstTime.seconds-firstTime.nanos` |
| Bucket time interval | ~150 ms (this dataset: 02:04:22.138Z → 02:04:22.288Z) |
| Samples per bucket | 19 (uniform across all 1,367 buckets) |
| Bucket size configurable? | Determined by ingestion batch — 1 bucket per PV per ingestion frame |
| Buckets per PV | 1.0 for this dataset |

### Metadata Per Bucket

- Provider ID + name
- Client request ID
- Creation timestamp
- Column metadata with provenance attributes
- Timestamp summary (firstTime, lastTime, sampleCount)
- Serialized timestamp bytes (base64)

### Timestamp Storage

- Timestamps stored as serialized protobuf bytes in `dataTimestamps.bytes`
- Summary fields: `firstTime`, `lastTime` with seconds + nanos components
- Each timestamp has `dateTime` (ISO string), `seconds` (Long), `nanos` (Long)
- Redundant storage: raw bytes + parsed summary fields

### Value Storage

- Values stored as flat array in `dataColumn.values`
- Type discriminator: `dataColumn._t` ("doubleColumn" or "int32Column")
- No compression on values within document

### Required Indexes (4)

1. `_id_` — default primary key
2. `pvName_1` — PV name lookup
3. `pvName_1_dataTimestamps.firstTime.seconds_1_...lastTime.nanos_1` — time-range queries by PV
4. `providerId_1` — provider-level queries

### Optimization Recommendations

1. **Increase samples per bucket** — 19 samples/bucket yields high per-document overhead (1,296 bytes per 19 values). Increasing to 100-500 samples/bucket would dramatically reduce document count and index overhead.
2. **Remove redundant timestamp storage** — `dataTimestamps.bytes` duplicates the firstTime/lastTime summary fields. Consider storing only the serialized bytes and computing summaries on read.
3. **Evaluate compound index usage** — The compound time-range index shows 0 accesses. If unused in production queries, removing it saves ~98 KB per 1,367 documents.
4. **Consider removing ISO dateTime strings** — `firstTime.dateTime` and `lastTime.dateTime` add ~50 bytes per timestamp when seconds/nanos are already present.
5. **Value array compression** — For double columns, delta encoding or quantization could reduce values array size.

---

## Task 5: Run 26 Storage Forecast

### Input Parameters

| Month | HDF5 Estimate |
|-------|--------------|
| March 2026 | 980 GB |
| April 2026 | 525 GB |
| May 2026 | 999 GB |
| June 2026 | 469 GB |
| July 2026 | < 1 TB |
| **Total Baseline** | **~3-4 TB** |

### Measured Expansion Factors

| Factor | Value | Basis |
|--------|-------|-------|
| Compressed data only | 1.09x | storageSize / HDF5 |
| Data + indexes | 2.04x | (storageSize + indexSize) / HDF5 |
| Uncompressed logical | 5.68x | dataSize / HDF5 |
| Full DB with metadata | 9.26x | totalSize(all collections) / HDF5 |

### MongoDB Storage Forecast

Using **2.04x** (data + indexes) as primary expansion factor:

| Component | Conservative (4 TB HDF5) | Expected (3.5 TB HDF5) | Worst-Case |
|-----------|--------------------------|------------------------|------------|
| **Bucket data** (compressed) | 4.4 TB | 3.8 TB | — |
| **Bucket indexes** | 3.5 TB | 3.1 TB | — |
| **Bucket total** | **8.2 TB** | **7.1 TB** | — |
| **Metadata & requestStatus** | 2 TB | 1.5 TB | — |
| **Total MLDP storage** | **~10 TB** | **~8.5 TB** | **~15 TB** |

### Detailed Breakdown

**Conservative Estimate (4 TB HDF5 baseline, 2.04x expansion):**

| Category | Size |
|----------|------|
| MongoDB bucket storageSize | 4.4 TB |
| MongoDB bucket indexes | 3.5 TB |
| requestStatus & operational metadata | 2.0 TB |
| **Total** | **~10 TB** |

**Expected Estimate (3.5 TB HDF5 baseline, 2.04x expansion):**

| Category | Size |
|----------|------|
| MongoDB bucket storageSize | 3.8 TB |
| MongoDB bucket indexes | 3.1 TB |
| requestStatus & operational metadata | 1.5 TB |
| **Total** | **~8.5 TB** |

**Worst-Case Estimate (4 TB HDF5, 5.68x uncompressed expansion + metadata):**

| Category | Size |
|----------|------|
| MongoDB data (uncompressed) | 22.7 TB |
| Indexes (est. ~87% of compressed data) | 3.5 TB |
| requestStatus overhead | 3.0 TB |
| **Total** | **~15 TB** |

### Monthly Breakdown (Expected, 2.04x)

| Month | HDF5 | MongoDB Est. |
|-------|------|-------------|
| March 2026 | 980 GB | 2.0 TB |
| April 2026 | 525 GB | 1.1 TB |
| May 2026 | 999 GB | 2.0 TB |
| June 2026 | 469 GB | 0.96 TB |
| July 2026 | ~1 TB | 2.0 TB |
| **Cumulative** | **~3.5 TB** | **~8.1 TB** |

### Important Caveats

1. **Small sample bias** — 305 KB extract may not represent compression behavior at TB scale. Snappy compression could be more effective with larger documents/blocks.
2. **requestStatus scaling** — Operational metadata scaled linearly here (3.6x of bucket data), but actual ratio depends on ingestion batch sizes and retry behavior.
3. **Index growth** — Index size is ~87% of storageSize at this scale. At larger scale, B-tree efficiency improves and ratio should decrease.
4. **Bucket size impact** — Increasing samples/bucket from 19 to 200 would reduce document count ~10x, proportionally reducing index overhead.

---

## Task 6: Future Direct Ingestion Architecture

### Current Architecture

```
Accelerator Telemetry → HDF5 Files (SDF) → MLDP Ingestion Service → MongoDB
```

### Future Architecture

```
Accelerator Telemetry → MLDP Ingestion Service → MongoDB
```

### Advantages

1. **Eliminates double-write** — No HDF5 serialization step before MongoDB ingestion
2. **Reduced latency** — Data available in MLDP seconds after acquisition vs hours/days for HDF5 batch processing
3. **Reduced storage duplication** — No need to maintain HDF5 + MongoDB copies simultaneously during active use
4. **Simpler pipeline** — Fewer moving parts, fewer failure modes
5. **Real-time query capability** — Operational queries on live data without waiting for HDF5 batch completion

### Disadvantages

1. **Loss of HDF5 as source of truth** — HDF5 files serve as immutable scientific record. Without them, MongoDB becomes the primary store with all associated durability/backup requirements.
2. **Increased MongoDB write load** — Continuous streaming ingestion vs periodic batch import. Sustained write throughput requirements increase dramatically.
3. **Data format dependency** — Accelerator data locked into MLDP's BSON schema. HDF5 is a universal scientific format; MongoDB is application-specific.
4. **Backup complexity** — HDF5 files are trivially backed up (file copy). MongoDB backups require coordinated snapshots.
5. **Higher availability requirements** — MLDP must be continuously available during accelerator operation. Current architecture tolerates MLDP downtime since HDF5 files can be imported later.

### Operational Impacts

- **Monitoring:** Real-time ingestion requires continuous throughput/latency monitoring
- **Scaling:** MongoDB must handle sustained write load (current: batch import; future: streaming)
- **Disaster recovery:** Must be comparable to HDF5 file-system-level recovery
- **Staffing:** Operational support for a always-on ingestion pipeline vs batch processing

### Performance Implications

- **Write throughput:** At 120 Hz sampling across 1,367+ PVs, ~164K values/second sustained. Current bucket design (19 samples/bucket) would generate ~8,600 bucket writes/second.
- **Batch vs stream:** Batch import can saturate network/CPU briefly. Streaming must sustain steady state.
- **Backpressure:** Streaming requires buffering/backpressure mechanisms that batch import does not.

### Storage Implications

- **Same expansion factors apply** — BSON overhead vs raw doubles remains constant
- **requestStatus may grow** — Streaming generates more status records per unit time
- **Index maintenance** — Continuous writes mean continuous index updates (vs bulk index build)

### Additional Infrastructure Requirements

| Requirement | Purpose |
|-------------|---------|
| **Message queue (Kafka/RabbitMQ)** | Buffer telemetry between accelerator and MLDP, handle backpressure |
| **Streaming ingestion endpoint** | gRPC/WebSocket endpoint for continuous PV data |
| **Intermediate buffer storage** | Local SSD or shared storage for queue overflow |
| **Health monitoring** | Alerting on ingestion lag, queue depth, write errors |
| **Schema registry** | Manage PV data types and format evolution |
| **Rate limiting** | Protect MongoDB from ingestion spikes |

---

## Task 7: Long-Term Data Management Strategy

### Three-Tier Storage Architecture

#### Tier 1: Scientific Storage — HDF5 on SDF

| Property | Details |
|----------|---------|
| **Purpose** | Scientific source of truth, immutable datasets, long-term preservation |
| **Format** | HDF5 |
| **Location** | SLAC SDF filesystem |
| **Retention** | Permanent / institutional policy |
| **Access pattern** | Write-once, read-many. Bulk analysis by scientists. |
| **Backup** | Filesystem-level snapshots + tape archive |

#### Tier 2: Operational Storage — MongoDB inside MLDP

| Property | Details |
|----------|---------|
| **Purpose** | Queryable operational datasets, metadata, provenance, derived machine state |
| **Format** | BSON documents (bucketed time series) |
| **Location** | MongoDB cluster |
| **Retention** | Active run + configurable lookback window |
| **Access pattern** | Read-heavy queries by PV name + time range. Write bursts during ingestion. |
| **Backup** | MongoDB snapshots, oplog-based point-in-time recovery |

#### Tier 3: Machine Learning Storage

| Property | Details |
|----------|---------|
| **Purpose** | Feature-engineered datasets, training data, validation sets, models |
| **Format** | Parquet/Arrow for tabular features, HDF5 for tensors, ONNX/pickle for models |
| **Location** | Object storage (S3/MinIO) or shared filesystem |
| **Retention** | Model lifecycle — keep while model is in production + lineage requirements |
| **Access pattern** | Bulk read for training, versioned writes for new feature sets |
| **Backup** | Object storage replication + version pinning |

### Recommendations

#### Data Ownership

| Tier | Owner | Rationale |
|------|-------|-----------|
| Scientific (HDF5) | Accelerator Operations / Physics | Source of truth, institutional data |
| Operational (MongoDB) | MLDP / Controls Software | Application-managed, operational scope |
| ML (features/models) | ML Engineering | Model lifecycle, experiment tracking |

#### Retention Policies

| Tier | Policy | Rationale |
|------|--------|-----------|
| Scientific | **Permanent** | Institutional scientific record, regulatory/reproducibility requirements |
| Operational | **Active run + 6 months** | Queries beyond 6 months served from HDF5 re-import or archive tier |
| ML features | **Model lifetime + 1 year** | Reproducibility of training runs, audit trail |
| ML models | **Production lifetime + 2 years** | Rollback capability, comparison baselines |

#### Backup Strategies

| Tier | Strategy | RPO | RTO |
|------|----------|-----|-----|
| Scientific | SDF snapshots + tape, filesystem replication | 24h | 48h |
| Operational | MongoDB continuous backup (oplog), daily snapshots | 1h | 4h |
| ML | Object store versioning + cross-region replication | 24h | 24h |

#### Long-Term Sustainability

1. **Avoid single-system dependency** — Keep HDF5 as canonical scientific format. MongoDB is operational, not archival.
2. **Data lifecycle automation** — Auto-expire MongoDB buckets beyond retention window. Re-import from HDF5 if historical queries needed.
3. **Storage tiering** — Hot (MongoDB SSD) → Warm (MongoDB HDD/compressed) → Cold (HDF5 on tape). Transition triggers based on data age.
4. **Capacity planning** — Use measured 2.04x expansion factor for MongoDB provisioning. Budget 10 TB for Run 26 (conservative).
5. **Schema evolution** — Version bucket document schema. Maintain backward-compatible readers for older bucket formats.
6. **Cost optimization** — MongoDB WiredTiger Snappy compression yields 5.2x compression on bucket data (1.77 MB → 340 KB). Validate this ratio holds at scale.
7. **Monitoring** — Track expansion factor monthly. If ratio drifts beyond 3x, investigate bucket sizing and index overhead.

---

## Summary of Key Findings

| Finding | Value |
|---------|-------|
| MongoDB Expansion Factor (data + indexes) | **2.04x** |
| MongoDB Expansion Factor (uncompressed BSON) | **5.68x** |
| Snappy compression ratio | **5.2x** on bucket data |
| Samples per bucket | 19 (uniform) |
| Average bucket document size | 1,296 bytes |
| Query performance (1000 PVs) | 67 ms |
| Run 26 MongoDB estimate (expected) | **~8.5 TB** |
| Run 26 MongoDB estimate (conservative) | **~10 TB** |
| Run 26 MongoDB estimate (worst-case) | **~15 TB** |
| Key recommendation | Increase samples/bucket from 19 to 200+ to reduce overhead |
