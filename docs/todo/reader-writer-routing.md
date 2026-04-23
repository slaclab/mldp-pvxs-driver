# TODO: Reader-to-Writer Routing in Controller

## Problem

Currently all readers feed all writers. Some writers should only receive data from specific readers (e.g., HDF5 writer stores BSAS tables only, gRPC writer forwards scalar PVs only). No routing mechanism exists in the controller.

## Goal

Add a fast, simple routing function in the controller that directs data from one or more readers to one or more writers. Each reader→writer link should be explicitly configurable.

## Requirements

- **Selective routing**: each writer declares which readers (or source patterns) it accepts
- **Many-to-many**: one reader can feed multiple writers; one writer can receive from multiple readers
- **Fast path**: routing decision must add negligible overhead per batch (lookup table, not regex per event)
- **Simple config**: expressible in YAML without complex syntax
- **Default behavior**: if no routing configured, all readers feed all writers (backward compatible)

## Proposed Configuration

```yaml
controller:
  routing:
    - reader: epics-archiver-1
      writers: [hdf5_local]
    - reader: bsas-reader
      writers: [hdf5_local, grpc_forwarder]
    - reader: scalar-reader
      writers: [grpc_forwarder]
```

When `routing` is absent or empty, every reader feeds every writer (current behavior).

## Design Sketch

1. **Route table**: `std::unordered_map<std::string, std::vector<WriterPtr>>` built at controller startup from config
2. **Controller dispatch**: when a reader produces a batch, controller looks up reader name in route table and pushes to matched writers only
3. **Validation**: at startup, verify all reader/writer names in routing config actually exist; fail fast on typo

## Open Questions

- Support glob/wildcard patterns for source-level routing (e.g., `sources: ["BSAS:*"]`)?
- Per-source routing vs. per-reader routing? Per-reader is simpler; per-source more flexible but heavier
- Hot-reload routing config without restart?

## Related Files

| File | Role |
|------|------|
| `include/controller/Controller.h` | Controller dispatches batches from readers to writers |
| `src/controller/Controller.cpp` | Current push-to-all-writers logic |
| `include/writer/Writer.h` | Writer interface (`push()`) |
| `include/reader/Reader.h` | Reader interface |
| `docs/configuration.md` | YAML config documentation |
