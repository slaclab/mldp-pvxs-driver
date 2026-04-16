# MLDP Query Client

`MLDPQueryClient` is the standalone client for querying MLDP gRPC for metadata and historical data. It is intentionally separate from `IDataBus`, which is now push-only for ingestion paths.

> **Related:** [Architecture Overview](architecture.md) | [Implementing Custom Writers](writers-implementation.md)

## Overview

`MLDPQueryClient` provides the query-side API used by tests, diagnostics, and tools that need to inspect MLDP data without participating in the reader-to-writer pipeline.

It lives in `include/query/impl/mldp/MLDPQueryClient.h` and implements the `IQueryable` interface.

## Constructor

The client is constructed with:

- `MLDPGrpcPoolConfig`
- optional `std::shared_ptr<metrics::Metrics>`

```cpp
MLDPQueryClient client(pool_config);
```

The constructor initializes the underlying query pool immediately.

## Query APIs

### `querySourcesInfo(source_names)`

Returns `std::vector<util::bus::IDataBus::SourceInfo>`.

- Accepts a `std::set<std::string>` of source names.
- Queries MLDP metadata for each source.
- Prefers the `queryPvMetadata` RPC and falls back to `queryData` when needed.

### `querySourcesData(source_names, options)`

Returns `std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>>`.

- Accepts a `std::set<std::string>` of source names.
- Accepts `util::bus::QuerySourcesDataOptions` for timeout and window tuning.
- Returns `std::nullopt` when transport or protocol failure prevents a result.

## Why It Is Separate from `IDataBus`

`IDataBus` is the ingestion-side push interface for readers and controllers.

`MLDPQueryClient` serves a different purpose:

- it performs out-of-band, on-demand queries
- it does not participate in the main push path
- it is useful for diagnostics, tests, and inspection tools

Keeping query traffic out of the bus keeps the ingestion path simpler and makes the bus implementation push-only.

## Example Usage

The best reference is `test/writer/grpc/mldp_grpc_writer_integration_test.cpp`, which exercises both metadata and data queries against a live MLDP backend.

Typical usage looks like this:

```cpp
MLDPQueryClient client(pool_config, metrics);

std::set<std::string> sources = {"MY:PV"};
auto infos = client.querySourcesInfo(sources);

util::bus::QuerySourcesDataOptions options;
auto data = client.querySourcesData(sources, options);
```

## Implementation Notes

- `MLDPQueryClient` is the canonical production query implementation.
- The interface is intentionally small so alternate backends can be injected later if needed.
- The query pool is separate from the ingestion pool so query traffic and write traffic can be managed independently.
