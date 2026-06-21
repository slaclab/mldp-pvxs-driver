# Set Data Provenance from Reader Configuration (P2)

## Priority

P2 — provenance metadata is needed for downstream traceability but not blocking current ingestion

## Problem

Readers currently have no standard way to declare data provenance (origin, facility,
instrument, subsystem) in their YAML configuration. Provenance metadata must be
hardcoded or inferred at runtime, making it difficult for downstream writers and
processors to tag data with its origin.

The `HDF5BsasGen1Reader` already supports a `metadata` map in its config, but this
is reader-specific and not part of the shared `Reader` interface or `EventBatch`
contract.

## Goal

Allow any reader to declare provenance metadata in its YAML configuration block.
This metadata should flow through to every emitted `EventBatch` and be available
to downstream writers and processors without reader-specific knowledge.

## Proposed Design

### 1. Add `provenance` section to reader YAML

```yaml
reader:
  - hdf5-bsas-gen1:
      - name: bsas_reader
        file-path: /data/bsas-export.h5
        provenance:
          facility: LCLS
          instrument: CXI
          subsystem: BSAS
          data-source: hdf5-file
```

### 2. Parse provenance in base `Reader` or shared config

Add a `provenance` field (map of string key-value pairs) to the base reader
configuration, parsed once and available to all reader implementations.

### 3. Attach provenance to `EventBatch`

Every `EventBatch` emitted by a reader should carry the provenance map in
`batch.metadata` (or a dedicated `batch.provenance` field if the team prefers
separation from arbitrary metadata).

### 4. Writer consumption

Writers that support provenance (e.g. HDF5Writer, MLDPWriter) should propagate
these key-value pairs into the output format — HDF5 file attributes, gRPC
metadata fields, or annotation service tags.

## Acceptance Criteria

- Any reader can declare `provenance:` in its YAML config block.
- Provenance key-value pairs appear in every `EventBatch` emitted by that reader.
- At least one writer (HDF5 or MLDP) persists provenance metadata to its output.
- Missing `provenance:` section is valid — defaults to empty map, no behavior change.
- Existing reader configs without `provenance:` continue to work unchanged.
- Unit test verifying provenance flows from config through reader to emitted batch.

## Files Likely Impacted

- `include/reader/Reader.h` — add provenance storage
- `include/config/Config.h` — helper for parsing provenance map
- `src/reader/Reader.cpp` — parse provenance from config
- `include/util/bus/IDataBus.h` — ensure `EventBatch` can carry provenance
- Reader implementations — pass provenance into emitted batches
- Writer implementations — consume and persist provenance
