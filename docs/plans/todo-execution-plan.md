# Execution Plan: PV Metadata + Queryable Factory + Annotation Client

## Purpose
Execution guidance for Claude Code when working through the todos in `docs/todo/task/`.
Each todo file contains full steps and commit message. This document adds: run order,
critical API facts, pre-read list, and common pitfalls.

---

## Execution Order

```
01 → 03 → 04
          ↑
02 ───────┘    (02 unblocks 03; 04 and 05 are independent of each other)
01 → 03 → 05

06 → 07 → 08 → 11
     ↑         ↑
     09 ←──────┘    (09 after 07; 11 after 08 and 10)
     10 ─────────→ 11
```

| # | Todo file | Depends on |
|---|-----------|------------|
| 01 | `docs/todo/task/01-event-batch-metadata.md` | — |
| 02 | `docs/todo/task/02-reader-config-metadata.md` | — |
| 03 | `docs/todo/task/03-reader-impl-metadata.md` | 01, 02 |
| 04 | `docs/todo/task/04-mldp-writer-metadata.md` | 01 |
| 05 | `docs/todo/task/05-hdf5-writer-metadata.md` | 01 |
| 06 | `docs/todo/task/06-queryable-factory.md` | — |
| 07 | `docs/todo/task/07-iqueryable-strip-and-client.md` | 06 |
| 08 | `docs/todo/task/08-controller-queryable-config.md` | 06, 07 |
| 09 | `docs/todo/task/09-factory-unit-tests.md` | 06, 07 |
| 10 | `docs/todo/task/10-grpc-annotation-pool.md` | — |
| 11 | `docs/todo/task/11-annotation-query-client.md` | 07, 08, 10 |

Parallel starts: **01 + 02 + 06 + 10** have zero dependencies — can all start simultaneously.

---

## Before Starting Any Todo

Read these files once per session. Every todo references them without repeating content:

```bash
# Core structs
include/util/bus/IDataBus.h                          # EventBatchStruct current state
include/writer/mldp/MLDPWriter.h                     # QueueItem struct
include/pool/MLDPGrpcPool.h                          # MLDPGrpcObject struct
include/pool/MLDPGrpcQueryPool.h                     # pool class pattern to mirror
include/pool/MLDPGrpcPoolConfig.h                    # config fields
include/query/IQueryable.h                           # current interface
include/query/impl/mldp/MLDPQueryClient.h            # current client header
src/controller/MLDPPVXSControllerConfig.cpp          # reader parsing pattern (lines 143–180)
src/config/Config.cpp                                # operator>> for map (line 237)
```

---

## Critical API Facts

### Config API
```cpp
cfg.hasChild("key")                        // existence check — NOT hasKey()
cfg.get("key", default)                    // scalar read
cfg.subConfig("key")                       // returns std::vector<Config>
cfg.isSequence("key")                      // true if key holds YAML sequence
cfg.subConfig("key").front() >> m          // read YAML map key into std::map<string,string>
```
`operator>>(std::map<string,string>&)` clears output then iterates children.
The receiver must be the map node itself — pass `subConfig(key).front()` for a single map key.

### Pool pattern (todos 10, 11)
`MLDPGrpcAnnotationPool` mirrors `MLDPGrpcQueryPool` exactly. Same `IObjectPool<T>` base,
same acquire/release/available. Define a new `MLDPGrpcAnnotationObject` struct (channel +
annotation stub) instead of reusing `MLDPGrpcObject`.

### QueryableFactory constraint (todos 06, 07, 08, 11)
`prepare<T>(cfg, metrics)` stores `make_unique<T>(cfg, metrics)`.
Every `T` registered with the factory **must** have:
```cpp
explicit T(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr);
```

### Controller config iteration pattern (todo 08)
Mirror `readerEntries_` in `src/controller/MLDPPVXSControllerConfig.cpp:143–180`.
Use `root.subConfig(key)` to get `vector<Config>`, iterate, call `node.get("type","")`.

---

## Build Verification After Each Todo

```bash
cmake --build build 2>&1 | grep -E "^.*error:" | grep -v "^In file included" | head -20
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

Both must be clean before committing.

---

## Common Pitfalls

| Pitfall | Prevention |
|---|---|
| `cfg.hasKey()` instead of `cfg.hasChild()` | `hasKey()` does not exist — always `hasChild()` |
| `subConfig("key").front()` on a sequence node | Only for map-valued keys; sequence keys require iteration |
| `batch.metadata` copy semantics | `staticMetadata()` returns const ref — copy first: `auto merged = config_.staticMetadata()` |
| `QueueItem.metadata` null dereference (todo 04) | Guard: `if (item.metadata && !item.metadata->empty())` |
| `prometheus::Labels tags` in MLDPWriter.cpp line ~357 | Different variable — do NOT rename it in todo 01 |
| `QueryableFactory` singleton state leaking between tests | Call `reset()` in both `SetUpTestSuite` AND `TearDownTestSuite` |
| `override` left on non-virtual methods after todo 07 | Grep for `override` on `querySourcesInfo`/`querySourcesData` after stripping IQueryable |
| HighFive duplicate attribute write (todo 05) | Gate on `seen_groups_.insert(...).second` — only write on first group creation |
