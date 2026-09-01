# Size Analysis: BSAS:SYS0:1:CUHXR_TBL Single-Event Storage

## Summary

A single EPICS event from the EPICS Gen-1 table `BSAS:SYS0:1:CUHXR_TBL` consumes
**8–9 MB in database storage** (13.4 MB as JSON export). This document explains why.

---

## Event Structure

| Property | Value |
|----------|-------|
| PV channels (columns) | 1,087 |
| Shots per event window | ~274 (varies 68–434) |
| Total scalar values | 297,471 |
| Timestamp type | per-column binary blob |
| Column data type | `doubleColumn` (float64) |

The NTTable aggregates **all beam-synchronous acquisition PVs** for LCLS-II copper
hard X-ray (CUHXR) beamline into one wide table. Each event captures a time window
of ~274 consecutive shots across all 1,087 channels simultaneously.

---

## Storage Breakdown (JSON export, 13.4 MB)

| Component | Size | % of total |
|-----------|------|-----------|
| Column values (doubles as JSON text) | 4.61 MB | 34.4% |
| Timestamp binary blobs (base64) | 5.66 MB | 42.2% |
| Per-bucket metadata (_id, clientRequestId, pvName, etc.) | 0.31 MB | 2.3% |
| Column metadata + names | 0.08 MB | 0.6% |
| JSON structural overhead (keys, braces, commas) | 2.74 MB | 20.5% |
| **Total** | **13.4 MB** | 100% |

### Estimated binary (BSON/database) size: ~8–9 MB

| Component | Size |
|-----------|------|
| Values (297,471 × 8 bytes) | 2.27 MB |
| Timestamp blobs (decoded from base64) | 3.89 MB |
| BSON key overhead + metadata × 1,087 docs | ~2–3 MB |
| **Total (estimated)** | **~8–9 MB** |

---

## Why Is One Event So Large?

### 1. Wide Table Design (Gen-1 architecture)

The Gen-1 BSAS table packs **1,087 PV channels** into a single NTTable structure.
This is the fundamental multiplier — every overhead cost is paid 1,087 times.

For comparison, if this were 10 separate tables of ~109 columns each, per-event
storage would be under 1 MB per table.

### 2. Per-Column Timestamp Duplication

The database stores each column as a **separate document (bucket)**, and each bucket
carries its own full timestamp blob:

```
Bucket size breakdown (single column, 274 shots):
  - values:     6,052 bytes (274 doubles as JSON)
  - timestamps: 5,464 bytes (protobuf-encoded binary, base64-wrapped)
  - metadata:     300 bytes (_id, pvName, clientRequestId, etc.)
  - total:     ~11,993 bytes
```

The timestamps encode the same shot-timing information for every column — yet are
stored independently 1,087 times. This accounts for **~3.9 MB of binary storage**
(5.66 MB in JSON) for information that could be shared once (~5 KB).

**Duplication factor: ~1,087×** for timestamp data alone.

### 3. JSON Text Expansion of Floating-Point Values

Each `float64` value occupies 8 bytes in binary but averages **22.1 characters** in
JSON text representation (e.g., `-0.023191551187403528`):

- Binary: 8 bytes/value
- JSON: 22.1 bytes/value (2.8× expansion)
- Base64 timestamps: 1.33× expansion over raw binary

This means the 2.27 MB of actual numeric data becomes 4.61 MB in JSON.

### 4. Structural Metadata Overhead Per Bucket

Each of the 1,087 documents carries:

```json
{
  "_id": "ACCL_IN20_300_L0A_PCUHBR-1781285330-131999941",
  "clientRequestId": "pv_stream_2162681498383757_BSAS:SYS0:1:CUHXR_TBL_0",
  "createdAt": {"$date": "2026-06-12T17:28:56.220Z"},
  "providerId": "...",
  "providerName": "...",
  "pvName": "..."
}
```

~300 bytes × 1,087 = 0.31 MB of repeated metadata.

### 5. NaN Filtering Removes Data but Not Schema Width

The CSV export shows **48.6% NaN values** — nearly half the channels have no valid
data in a given event. However, the database storage model still creates a bucket
for every column regardless. Sparse columns with mostly-NaN data still carry the
full timestamp blob overhead.

---

## Comparison: Raw Data vs Stored Size

| Representation | Size | Expansion vs raw |
|----------------|------|-----------------|
| Raw binary (values only) | 2.27 MB | 1× |
| Raw binary (values + shared timestamps) | 2.28 MB | 1.004× |
| Database BSON (per-column buckets) | ~8–9 MB | 3.5–4× |
| JSON export | 13.4 MB | 5.9× |

---

## Root Cause Summary

```
Single event = 1,087 columns × 274 shots = 297,471 values

Actual numeric payload:     2.27 MB (what you care about)
Duplicated timestamps:     +3.89 MB (same timing × 1,087)
Per-bucket overhead:       +2.5 MB  (IDs, keys, BSON structure)
                           --------
Database total:            ~8.7 MB  (for 2.27 MB of useful data)
```

The storage efficiency is approximately **26%** — only 1/4 of stored bytes are
actual measurement values.

---

## Potential Optimizations

| Approach | Estimated Savings |
|----------|------------------|
| Share timestamps once per event (not per column) | −3.8 MB (−44%) |
| Skip empty/NaN-only columns | −1–2 MB (depends on sparsity) |
| Store values as binary arrays (not JSON doubles) | −2.3 MB in export |
| Reduce column count (split into smaller tables) | Linear reduction |
| Compress binary blobs (zstd/lz4) | 2–3× on numeric arrays |

The single highest-impact change: **deduplicate timestamps** (store once per event
instead of once per column). This alone would reduce the ~8.7 MB event to ~4.9 MB.

---

## Conclusion

The large per-event size is an emergent property of three factors compounding:

1. **Gen-1 wide-table design** — 1,087 columns in one NTTable
2. **Per-column storage model** — timestamps and metadata duplicated per column
3. **~50% sparse data** — NaN columns still consume structural overhead

The actual scientific payload (measurement doubles) is only 2.27 MB. The remaining
6–7 MB is structural overhead from the storage model and timestamp duplication.
