# TODO: Writer / Reader Infrastructure Documentation

## Goal

Create reference documentation for the pluggable writer and reader infrastructure so
new contributors can add a writer or reader type without reading all the source code.
Update `docs/readers-implementation.md` to add a "Writers" section that mirrors its
existing reader coverage.

---

## Tasks

### 1. Create `docs/writers-implementation.md`
Cover:
- `IWriter` interface (`start`, `stop`, `push`)
- `WriterFactory` registry — how `REGISTER_WRITER` macro registers a type at
  static-init time; how `WriterFactory::create(type, node, metrics)` dispatches
- `WriterRegistrator<T>` template and the `static inline` member pattern
- `writerEntries()` on `MLDPPVXSControllerConfig` — how the controller iterates
  configured writer types without any `#ifdef`
- Step-by-step guide: "How to add a new writer type"
  1. Implement `IWriter`
  2. Add `(const config::Config&, shared_ptr<Metrics>)` constructor
  3. Add `REGISTER_WRITER("my-type", MyWriter)` inside the class
  4. Add `src/writer/my_type/MyWriter.cpp` to `CMakeLists.txt`
  5. Update `WriterConfig` / `MLDPPVXSControllerConfig` YAML parsing if the new
     type needs its own YAML sub-block

### 2. Update `docs/readers-implementation.md`
- Add a short cross-reference section pointing to `docs/writers-implementation.md`
- Note that `ReaderFactory` and `WriterFactory` are intentional mirrors; link to
  the factory-unification TODO for future consolidation

### 3. Create `docs/query-client.md`
Cover:
- `MLDPQueryClient` — standalone class for querying MLDP gRPC for metadata and
  historical data; not part of `IDataBus`
- Constructor: takes `MLDPGrpcPoolConfig` + optional `Metrics`
- `querySourcesInfo(source_names)` — returns `vector<SourceInfo>`
- `querySourcesData(source_names, options)` — returns optional map of
  `DataValues` per source
- Example usage (see `test/pool/mldp_grpc_pool_test.cpp`)
- Why it was separated from `IDataBus`: the bus is now push-only; query is an
  out-of-band, on-demand operation not needed by readers

### 4. Update `docs/architecture.md`
- Replace any mention of `IDataBus::querySourcesInfo/Data` with `MLDPQueryClient`
- Add `WriterFactory` to the component diagram or component list

---

## Key files

| File | Role |
|---|---|
| `include/writer/WriterFactory.h` | Factory + macro |
| `include/writer/IWriter.h` | Writer interface |
| `include/query/MLDPQueryClient.h` | Query client |
| `include/util/bus/IDataBus.h` | Push-only bus interface |
| `docs/readers-implementation.md` | Existing reader docs to update |
| `docs/architecture.md` | Architecture overview to update |
