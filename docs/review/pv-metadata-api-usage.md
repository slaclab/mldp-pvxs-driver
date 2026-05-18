# PvMetadata API Usage Review

**Date**: 2026-05-18  
**Scope**: Which API layers (ingestion, query, annotation) use `PvMetadata` and related structures, and how to populate PV metadata.

---

## 1. Definitions

There is no single `PvMetadata` type. Three distinct concepts share the name or purpose across layers.

### 1.1 `dp.service.common.PvMetadata` — Annotation Record

**File**: `build/_deps/dp_grpc-src/src/main/proto/common.proto:72`

```proto
message PvMetadata {
  string pvName = 1;
  repeated string aliases = 2;
  repeated string tags = 3;
  repeated Attribute attributes = 4;
  Timestamp createdTime = 5;
  Timestamp updatedTime = 6;
  string modifiedBy = 7;
  string description = 8;
}
```

Durable, user-managed record for PV identity and classification. Server sets audit fields (`createdTime`, `updatedTime`). Not time-series data.

### 1.2 `QueryPvStatsResponse.StatsResult.PvStats` — Query Archive Stats

**File**: `build/_deps/dp_grpc-src/src/main/proto/query.proto:350`

```proto
message PvStats {
  string pvName = 1;
  string lastBucketId = 2;
  string lastBucketDataType = 3;
  int32  lastBucketDataTimestampsCase = 4;
  string lastBucketDataTimestampsType = 5;
  uint32 lastBucketSampleCount = 6;
  uint64 lastBucketSamplePeriod = 7;     // nanoseconds; 0 if TimestampList
  Timestamp firstDataTimestamp = 8;
  Timestamp lastDataTimestamp = 9;
  int32  numBuckets = 10;
  string lastProviderId = 11;
  string lastProviderName = 12;
}
```

Per-PV archive summary returned by the `queryPvStats` RPC. Reflects what is stored in the archive (bucket inventory), not user-managed metadata.

> **Name history**: The RPC was previously named `queryPvMetadata` / `QueryPvMetadataResponse`. Renamed to `queryPvStats` in commit `141d55f`. The `build_test/` dependency snapshot still carries the old name; the production `build/` dependency uses the new name.

### 1.3 `ColumnMetadata` / `ColumnProvenance` — Ingestion Column Tags

**File**: `build/_deps/dp_grpc-src/src/main/proto/common.proto:27`

```proto
message ColumnProvenance { string source = 1; string process = 2; }
message ColumnMetadata {
  ColumnProvenance provenance = 1;
  repeated string tags = 2;
  repeated Attribute attributes = 3;
}
```

Attached to every typed column in `IngestDataRequest`. Carries lineage (`source`, `process`) and optional classification metadata per column.

### 1.4 `IDataBus::SourceInfoStruct` — In-Process C++ Representation

**File**: `include/util/bus/IDataBus.h:57`

```cpp
struct SourceInfoStruct {
    std::string                    source_name;
    std::optional<SourceTimestamp> first_timestamp;
    std::optional<SourceTimestamp> last_timestamp;
    std::optional<std::string>     last_provider_id;
    std::optional<std::string>     last_provider_name;
    std::optional<std::string>     last_bucket_id;
    std::optional<std::string>     last_bucket_data_type;
    std::optional<std::string>     last_bucket_data_timestamps_type;
    std::optional<uint64_t>        last_bucket_sample_period;
    std::optional<uint32_t>        last_bucket_sample_count;
    std::optional<int32_t>         num_buckets;
};
using SourceInfo = SourceInfoStruct;
```

Normalized in-process mirror of `PvStats`. Decouples internal logic from protobuf types.

---

## 2. Ingestion Path

`PvMetadata` (§1.1) is **not used** in ingestion. What flows is `ColumnMetadata.ColumnProvenance.source`.

**File**: `src/writer/mldp/MLDPWriter.cpp:491–624`

For every typed column being packed into an `IngestDataRequest`, the writer stamps:

```cpp
c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
```

`rootSource` comes from `EventBatchStruct::root_source`, which readers set from the PV name. This is repeated for all column types: Int32, Int64, Double, Float, Bool, String, Enum, and their array variants.

### Planned Extension (not yet implemented)

Commit `9ff4252` and plan `docs/plans/static-metadata-readers.md` describe adding:

- `static_metadata_` field on reader configs (YAML `static_metadata: {key: value, ...}`)
- `PVConfig::metadata` for per-PV overrides
- Merging both into `EventBatch::metadata` (replacing the unused `tags` vector)
- Forwarding merged entries to `ColumnMetadata.attributes` at ingestion

Current state: the `EventBatchStruct::tags` vector is declared in `IDataBus.h:36` but is not written to anywhere. The `metadata` map migration is pending.

---

## 3. Query Path

### RPC

`DpQueryService.queryPvStats` — production proto (`build/`)  
`DpQueryService.queryPvMetadata` — old name, test-snapshot proto (`build_test/`)

### C++ Interface

**File**: `include/query/IQueryable.h`

```cpp
class IQueryable {
    virtual std::vector<IDataBus::SourceInfo>
    querySourcesInfo(const std::set<std::string>& source_names) = 0;

    virtual std::optional<std::unordered_map<std::string, std::vector<DataValues>>>
    querySourcesData(const std::set<std::string>& source_names,
                     const QuerySourcesDataOptions& options = {}) = 0;
};
```

### Implementation Flow

**Files**: `include/query/impl/mldp/MLDPQueryClient.h`, `src/query/MLDPQueryClient.cpp:122–308`

`querySourcesInfo()` steps:

1. Build `QueryPvStatsRequest` with `PvNameList` of requested source names.
2. Send RPC with 5-second deadline.
3. On `UNIMPLEMENTED` (server too old): fall back to `queryData` over `[epoch=0, now+1s]`, synthesise `SourceInfo` from bucket scan.
4. On success: map each `PvStats` proto to `SourceInfoStruct` field-by-field (lines 279–301).

### Test Coverage

**File**: `test/writer/mldp/mldp_writer_integration_test.cpp:577–608`

After ingestion, polls `querySourcesInfo` for up to 10 seconds and asserts `last_timestamp` is populated. Comment at line 600 documents graceful degradation when `queryPvMetadata` (old RPC name) is not available.

---

## 4. Annotation Path — How to Populate PV Metadata

**This is the correct API to populate PV metadata.** Use `DpAnnotationService.savePvMetadata`.

### RPC Set (`DpAnnotationService`)

**File**: `build/_deps/dp_grpc-src/src/main/proto/annotation.proto:25`

| RPC | Semantics | Status |
|-----|-----------|--------|
| `savePvMetadata` | Full-replace upsert (create or replace) | Implemented |
| `queryPvMetadata` | Multi-criterion search with pagination | Implemented |
| `getPvMetadata` | Single-record lookup by PV name or alias | Implemented |
| `deletePvMetadata` | Delete by PV name or alias | Implemented |
| `patchPvMetadata` | Partial update via field mask | **NOT YET IMPLEMENTED** (stub only) |
| `bulkSavePvMetadata` | Batch full-replace upsert | **NOT YET IMPLEMENTED** (stub only) |

---

### Writing Metadata — `savePvMetadata`

**Proto**: `annotation.proto:809`

```proto
message SavePvMetadataRequest {
  string pvName = 1;                         // required — canonical PV name (primary key)
  repeated string aliases = 2;               // optional alternate / historical names
  repeated string tags = 3;                  // optional; normalized to lowercase unique set
  repeated dp.service.common.Attribute attributes = 4; // optional key/value pairs
  string modifiedBy = 5;                     // optional actor identity
  string description = 6;                    // optional free-text description
}
```

Where `Attribute` is (`common.proto:21`):
```proto
message Attribute {
  string name = 1;   // key
  string value = 2;  // value
}
```

**CRITICAL — full-replace semantics**: every `savePvMetadata` call replaces ALL fields atomically. Fields omitted from the request are erased — not preserved. Callers must supply the complete desired state on every call. `patchPvMetadata` (partial update) is defined but not yet implemented.

**Response**: `SavePvMetadataResponse` with `oneof result`:
- `exceptionalResult` — rejection or error (check this first)
- `savePvMetadataResult.pvName` — canonical PV name of created/updated record

#### Minimal C++ example (gRPC stub pattern)

```cpp
// build channel to annotation service
auto channel = grpc::CreateChannel(annotation_endpoint, creds);
auto stub = dp::service::annotation::DpAnnotationService::NewStub(channel);

dp::service::annotation::SavePvMetadataRequest req;
req.set_pvname("KLYS:LI22:21:PHAS");
req.add_aliases("LI22_21_KLYS_PHAS");
req.add_tags("klystron");
req.add_tags("linac");
auto* attr = req.add_attributes();
attr->set_name("sector");
attr->set_value("22");
req.set_modifiedby("mldp-pvxs-driver");
req.set_description("Klystron LI22-21 phase");

dp::service::annotation::SavePvMetadataResponse resp;
grpc::ClientContext ctx;
ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

auto status = stub->savePvMetadata(&ctx, req, &resp);
if (!status.ok()) { /* gRPC transport error */ }
if (resp.result_case() == SavePvMetadataResponse::kExceptionalResult) { /* service error */ }
// success: resp.savepvmetadataresult().pvname()
```

---

### Reading Back Metadata

#### By PV name — `getPvMetadata`

**Proto**: `annotation.proto:990`

```proto
message GetPvMetadataRequest {
  string pvNameOrAlias = 1;  // canonical name or any registered alias
}
```

Response payload: `GetPvMetadataResult.pvMetadata` → `dp.service.common.PvMetadata` (includes server-set `createdTime`, `updatedTime`).

Returns `ExceptionalResult` (not an empty record) when PV not found.

#### Structured search — `queryPvMetadata`

**Proto**: `annotation.proto:873`

```proto
message QueryPvMetadataRequest {
  repeated QueryPvMetadataCriterion criteria = 1;  // multiple criteria = AND
  uint32 limit = 2;
  string pageToken = 3;  // omit on first call; use nextPageToken from previous response
}
```

Criterion types (set exactly one per `QueryPvMetadataCriterion`):

| Criterion | Match logic | Fields |
|-----------|-------------|--------|
| `PvNameCriterion` | OR across `exact` / `prefix` / `contains` sub-lists | `exact[]`, `prefix[]`, `contains[]` |
| `AliasesCriterion` | same as PvNameCriterion but against aliases | `exact[]`, `prefix[]`, `contains[]` |
| `TagsCriterion` | any tag in list matches (OR) | `values[]` |
| `AttributesCriterion` | key required; empty `values` = key-existence check | `key`, `values[]` |

Multiple criteria entries in the outer list are combined with AND. An empty criteria list is rejected.

**Pagination**: response contains `pvMetadataResult.nextPageToken`; empty string = last page.

---

### Deleting Metadata — `deletePvMetadata`

**Proto**: `annotation.proto:1065`

```proto
message DeletePvMetadataRequest {
  string pvNameOrAlias = 1;
}
```

Response: `DeletePvMetadataResult.pvName` on success, `ExceptionalResult` on error/not-found.

---

### Driver Integration Status

**No annotation client exists yet in this driver.** There are no `#include <annotation.pb.h>` or `dp::service::annotation::` references anywhere in `/src`, `/include`, or `/test`.

To add annotation support, the driver needs:
1. Link against the annotation stub library (already in `dp_grpc` CMake target)
2. Add an `IAnnotationClient` interface (mirror of `IQueryable` pattern)
3. Implement `MLDPAnnotationClient` using `DpAnnotationService::NewStub`
4. Wire into the reader pipeline to call `savePvMetadata` on PV registration or config load

---

## 5. Summary

| Type | API Layer | Defined in | Used in Driver |
|------|-----------|------------|----------------|
| `dp.service.common.PvMetadata` | Annotation | `common.proto:72` | **No — needs implementation** |
| `DpAnnotationService.savePvMetadata` | Annotation (write) | `annotation.proto:91` | **No — needs implementation** |
| `DpAnnotationService.queryPvMetadata` | Annotation (read) | `annotation.proto:101` | **No — needs implementation** |
| `QueryPvStatsResponse.PvStats` | Query | `query.proto:350` (build/) | Yes — `MLDPQueryClient.cpp` |
| `ColumnMetadata` / `ColumnProvenance` | Ingestion | `common.proto:27` | Yes — `MLDPWriter.cpp` (`.source` only) |
| `IDataBus::SourceInfo` | Internal C++ | `IDataBus.h:57` | Yes — query client + integration test |
| `EventBatchStruct::tags` | Internal C++ | `IDataBus.h:36` | Declared, not written |
| `static_metadata_` / `PVConfig::metadata` | Config / Ingestion | Planned | Not yet implemented |

### Key Observations

1. **To populate PV metadata: use `DpAnnotationService.savePvMetadata`.** It is an upsert (create or replace) keyed by `pvName`. Supply all desired fields on every call — partial updates are not supported yet (`patchPvMetadata` is a stub only).

2. **Annotation layer is wired in proto but dead in C++.** Driver ingests and queries but never calls any annotation RPC. Requires adding an annotation client (see §4 integration steps).

3. **Two separate "metadata" concepts exist — don't conflate them:**
   - `ColumnMetadata.ColumnProvenance.source` — ingestion-time lineage, stamped per column, not user-editable.
   - `dp.service.common.PvMetadata` — annotation-layer record, user-managed, queryable by name/tag/attribute.

4. **`ColumnMetadata` is partially used.** Only `provenance.source` is stamped. `tags`, `attributes`, and `provenance.process` are always empty. The planned static-metadata feature (`docs/plans/static-metadata-readers.md`) would fill `attributes` at ingestion time — but that is separate from the annotation-layer `PvMetadata`.

5. **Query uses the renamed `queryPvStats` RPC** with fallback for older servers. Both names handled transparently in `MLDPQueryClient.cpp`.
