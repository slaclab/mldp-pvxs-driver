# TODO-04: Forward `batch.metadata` into column provenance labels in MLDPWriter

## Goal
Each column written by `MLDPWriter` should carry the batch's `metadata` map as
provenance labels in the protobuf `ColumnMetadata.provenance.labels` map.

## Depends On
- TODO-01 (`batch.metadata` exists; `QueueItem::tags` is now `metadata`)

## Context
`MLDPWriter` uses an internal `QueueItem` struct that currently carries
`shared_ptr<const vector<string>> tags` (updated in TODO-01 to
`shared_ptr<const unordered_map<string,string>> metadata`).

The column-building code (around line 491+) already calls:
```cpp
c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
```

We need to also populate `labels` on the provenance object.

## Before doing this
1. Verify the exact protobuf accessor for `labels`. Open the generated header or `.proto` file:
   ```bash
   find build/ -name "common.pb.h" | head -1 | xargs grep -n "labels\|mutable_provenance\|Provenance" | head -20
   ```
   Expected pattern: `(*prov->mutable_labels())[k] = v`

## Files to Change

### `include/writer/mldp/MLDPWriter.h`
- `QueueItem::tags` field was already renamed to `metadata` in TODO-01
- Confirm type is `shared_ptr<const unordered_map<string,string>>`

### `src/writer/mldp/MLDPWriter.cpp`
- In `toDataFrame()` or equivalent column-building function, locate every call to
  `set_source(rootSource)`. After each one, add:
  ```cpp
  if (item.metadata) {
      auto* prov = c->mutable_metadata()->mutable_provenance();
      for (auto& [k, v] : *item.metadata)
          (*prov->mutable_labels())[k] = v;
  }
  ```
  (If there are many repeated blocks, consider a small lambda to avoid repetition.)

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Check with MLDP integration test (if available) or inspect gRPC payload via log
ctest --test-dir build -R "mldp_writer" -V 2>&1 | tail -30
```

## Commit
```
feat(metadata): forward batch metadata as column provenance labels in MLDPWriter

Each column's ColumnMetadata.provenance.labels map is now populated with
the key-value pairs from EventBatch::metadata, carrying reader and PV
static metadata through to the MLDP ingestion service.
```
