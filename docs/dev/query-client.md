# Query Clients

The driver provides out-of-band query clients for inspecting MLDP metadata and data. These are intentionally separate from `IDataBus`, which is push-only for ingestion.

> **Related:** [Architecture Overview](../reference/architecture.md) | [Configuration Reference](../guides/configuration.md#queryable-block) | [Writers Overview](../writers/writers-implementation.md)

---

## QueryableFactory

`QueryableFactory` is a singleton registry that decouples client creation from configuration. Clients are prepared once at startup; components create fresh instances on demand.

### Lifecycle

1. **Startup** — `MLDPPVXSController::start()` calls `prepareQueryables()`, which iterates the `queryable:` config block and calls `QueryableFactory::instance().prepare<T>(cfg, metrics)` for each known type.
2. **Runtime** — any component calls `QueryableFactory::instance().create<T>()` to get a `unique_ptr<T>` constructed from the stored config closure.

```cpp
// Startup (inside controller):
QueryableFactory::instance().prepare<MLDPQueryClient>(cfg, metrics);

// Runtime (in a reader or test):
auto client = QueryableFactory::instance().create<MLDPQueryClient>();
auto infos = client->querySourcesInfo({"MY:PV"});
```

### Supported types

| Type key (YAML) | Class | Header |
|---|---|---|
| `mldp` | `MLDPQueryClient` | `include/query/impl/mldp/MLDPQueryClient.h` |
| `mldp-annotation` | `MLDPAnnotationQueryClient` | `include/query/impl/mldp/MLDPAnnotationQueryClient.h` |

`QueryableFactory::isPrepared<T>()` returns `true` when the type has been registered. Calling `create<T>()` on an unprepared type throws `std::runtime_error`.

### IQueryable interface

All query clients implement `IQueryable` (`include/query/IQueryable.h`). The interface is intentionally small so alternate backends can be injected in tests.

---

## MLDPQueryClient

Queries source metadata and historical data from the MLDP gRPC backend.

### Configuration

Prepared under `queryable.mldp`:

```yaml
queryable:
  mldp:
    mldp-pool:
      ingestion-url: grpc://ingest:50051
      query-url:     grpc://query:50052
      min-conn: 1
      max-conn: 2
```

### Query APIs

#### `querySourcesInfo(source_names)`

Returns `std::vector<util::bus::IDataBus::SourceInfo>`.

- Accepts `std::set<std::string>` of source names.
- Calls `queryPvMetadata` RPC (falls back to `queryData` when needed).

#### `querySourcesData(source_names, options)`

Returns `std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>>`.

- Accepts `std::set<std::string>` source names and `util::bus::QuerySourcesDataOptions` (timeout, window).
- Returns `std::nullopt` on transport or protocol failure.

### Example

```cpp
auto client = QueryableFactory::instance().create<MLDPQueryClient>();

std::set<std::string> sources = {"MY:PV"};
auto infos = client->querySourcesInfo(sources);

util::bus::QuerySourcesDataOptions options;
auto data = client->querySourcesData(sources, options);
```

---

## MLDPAnnotationQueryClient

Queries source metadata from the MLDP `DpAnnotationService` gRPC backend.

### Configuration

Prepared under `queryable.mldp-annotation`:

```yaml
queryable:
  mldp-annotation:
    mldp-annotation-pool:
      annotation-url: grpc://annotation-host:50053
      min-conn: 1
      max-conn: 2
```

### Example

```cpp
auto client = QueryableFactory::instance().create<MLDPAnnotationQueryClient>();
// use client->queryPvMetadata(…) or other IQueryable methods
```

---

## Why Separate from IDataBus

`IDataBus` is the push interface for readers. Query clients serve a different purpose:

- out-of-band, on-demand queries (diagnostics, tests, inspection tools)
- do not participate in the main push path
- can be created independently of the ingestion pipeline

Keeping query traffic out of the bus keeps the ingestion path push-only and free of two-way gRPC state.
