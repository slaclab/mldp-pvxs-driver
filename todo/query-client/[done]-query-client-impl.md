# query-client-impl — Embedded Query Engine over gRPC Virtual Tables

`mldp_pvxs_driver` in query mode is a **real embedded database** whose tables are backed
by remote gRPC services instead of local storage. The query engine runs entirely in-process:
SQL parsing, multi-pass planning, columnar execution, disk-spill for large joins.

Future: expose a TCP port (`--serve-flight`) and the same engine becomes a network-accessible
query service via Arrow Flight SQL — no architectural changes required.

Implementation follow-up completed: [Phase 10 — Final Server-Cursor Streaming Acceptance]([done]-phase-10-final-streaming-acceptance.md).

---

## Technology Stack

| Layer | Library | Rationale |
|---|---|---|
| Internal columnar format | **Apache Arrow C++** (`arrow`) | Zero-copy column batches; IPC = spill wire format; foundation for Flight |
| Spill / temp table storage | **`arrow::fs::FileSystem`** | Swappable FS abstraction: local today, S3/GCS tomorrow; `MockFileSystem` for tests |
| Spill file format | **Arrow IPC (`arrow::ipc`)** | Native Arrow serialisation; no schema conversion; random-access via memory-map |
| Future TCP wire | **Arrow Flight (`arrow::flight`)** | Streams `RecordBatch` over gRPC; same physical plan, different output sink |
| Memory accounting | **`arrow::MemoryPool`** | All allocations tracked; spill triggers when pool exceeds `--memory-mb` |

### Why `arrow::fs::FileSystem` as the "Virtual Filesystem"

`arrow::fs::FileSystem` is Arrow's own pluggable FS abstraction — the right choice for C++:

- **`arrow::fs::LocalFileSystem`** — default, writes spill files to `--spill-dir` (default: `std::filesystem::temp_directory_path()`)
- **`arrow::fs::SubTreeFileSystem`** — restricts all I/O to a single directory (the spill dir); prevents path traversal
- **`arrow::fs::MockFileSystem`** — fully in-memory, used in unit tests without touching disk
- **Future**: `arrow::fs::S3FileSystem` / `arrow::fs::GcsFileSystem` — swap the FS backend without changing any executor code

The `SpillManager` owns one `arrow::fs::FileSystem` instance (injected at startup).
Every spill file is opened/read/deleted through it. Swapping local → S3 = one constructor change.

---

---

## SQL-like Query Language Design

### Core Concept

Each queryable backend is modelled as a **virtual table** with a dotted namespace.
The query language is a strict subset of SQL `SELECT`: joins, derived sources, and
uncorrelated `IN (SELECT ...)` predicates are supported; scalar subqueries and DDL
are not part of the core query grammar.
The parser is hand-written (no external SQL library dependency) — grammar is simple enough.

### Virtual Table Registry

Each `IQueryable` subclass declares which virtual tables it owns via a static trait:

```cpp
// New static method required on every IQueryable implementation
static constexpr std::string_view virtualTable() { return "mldp.time_series"; }
```

The `QueryableFactory` (already a singleton registry) is extended to also map
`table_name → creator`, populated at `prepare<T>()` time by reading `T::virtualTable()`.

At query time, the parser extracts the `FROM <table>` clause, the factory resolves the
correct `IQueryable` instance, and the WHERE predicates are translated into that
backend's native parameter struct.

### Virtual Table Catalogue

| Virtual Table | Backed By | Queryable Class |
|---|---|---|
| `mldp.time_series` | `querySourcesData` | `MLDPQueryClient` |
| `mldp.pv_stats` | `querySourcesInfo` | `MLDPQueryClient` |
| `mldp.pv_metadata` | `queryPvMetadata` / `getPvMetadata` | `MLDPAnnotationQueryClient` |
| `mldp.configuration` | `queryConfigurations` / `getConfiguration` | `MLDPAnnotationQueryClient` |
| `mldp.configuration_activation` | `queryConfigurationActivations` | `MLDPAnnotationQueryClient` |
| `mldp.active_configurations` | `getActiveConfigurations` | `MLDPAnnotationQueryClient` |

### Grammar (EBNF)

```
query        ::= SELECT column_list FROM table_ref [join_clause*]
                 [WHERE predicate_list]
                 [LIMIT integer] [PAGE TOKEN string]

column_list  ::= '*' | qualified_column (',' qualified_column)*
qualified_column ::= [alias '.'] identifier

table_ref    ::= table_name [AS alias]
table_name   ::= identifier ('.' identifier)*        -- e.g. mldp.time_series
alias        ::= identifier

join_clause  ::= join_type JOIN table_ref ON join_condition
join_type    ::= INNER | LEFT [OUTER]
join_condition ::= qualified_column '=' qualified_column  -- equi-join only

predicate_list ::= predicate (AND predicate)*         -- only AND, no OR at top level

predicate    ::= qualified_column op value
               | qualified_column IN '(' (value_list | select_statement) ')'
               | qualified_column LIKE string_literal
               | qualified_column BETWEEN value AND value

op           ::= '=' | '!=' | '<' | '<=' | '>' | '>=' | 'CONTAINS' | 'PREFIX'
value        ::= string_literal | number | 'NOW' | 'NOW' ('+' | '-') duration
duration     ::= number ('s' | 'm' | 'h')
value_list   ::= value (',' value)*
```

Only `AND` at top level. `OR` within a single criterion via `IN (...)`. An
`IN (SELECT ...)` child must return exactly one non-null column with values
compatible with the target column. The child is evaluated once before its
dependent scan. `mldp.time_series_table` keeps
`window IN (SELECT time, end_time ...)` as a table-specific two-column interval
input; it is not scalar membership.
Only equi-joins (`ON t1.col = t2.col`) — no theta-joins, no cross-joins, no self-joins.
Only `INNER JOIN` and `LEFT OUTER JOIN` — no RIGHT/FULL (all backends are ordered drive/probe).

### Join Fundamentals: Why All Joins Are Client-Side

Every virtual table is a **separate gRPC service** — there is no shared storage layer,
no query federation protocol, and no server-side join execution. The planner must therefore
always fetch both sides independently and join in the executor memory.

This is a hard constraint that shapes every join optimisation decision:

1. No join can be pushed to a backend
2. Result set size determines algorithm choice (hash vs nested-loop)
3. The drive/probe order matters for `LEFT JOIN` correctness and for performance
4. Predicate pushdown still applies per-side before the join

### Join Algorithm Selection (Planner Decision)

The planner selects the physical join algorithm based on the estimated cardinality of each
side. Cardinality is not available from remote backends without a separate `COUNT` RPC
(too expensive), so the planner uses a **heuristic**: predicates with `required=true`
columns that use point-lookup ops (`=`, `IN`) are classified as **bounded scans**;
all others are **unbounded scans**.

| Left side | Right side | Algorithm chosen |
|---|---|---|
| bounded | bounded | **Hash Join** (smaller side builds hash table) |
| bounded | unbounded | **Hash Join** (left builds, right probes) |
| unbounded | bounded | **Hash Join** (right builds, left probes) — sides swapped |
| unbounded | unbounded | **Hash Join** with memory cap → spill warning |

**Nested-Loop Join** is used only when a correlated push is possible (see below). Not the
default — it is O(n×m) and every inner iteration is an RPC.

**Memory cap**: hash table is capped at a configurable `--join-memory-mb` (default: 256 MB).
If the build side exceeds the cap, the executor warns and switches to a block-nested-loop
with multiple passes over the probe side, each pass scanning a chunk of the build side.

### Join Physical Plan Nodes

```cpp
enum class JoinType { INNER, LEFT_OUTER };

enum class JoinAlgorithm {
    HASH,              // standard in-memory hash join (default)
    NESTED_LOOP,       // correlated push: inner scan re-fetched per outer row
    BLOCK_NESTED_LOOP, // fallback when hash table exceeds memory cap
};

struct JoinCondition {
    std::string left_table_alias;
    std::string left_column;
    std::string right_table_alias;
    std::string right_column;
};

struct PhysicalHashJoin {
    JoinType        type;
    JoinCondition   condition;
    PhysicalNodePtr build_side;         // smaller/bounded side — loaded into hash table
    PhysicalNodePtr probe_side;         // larger/unbounded side — streamed
    uint32_t        spill_partitions;   // N partitions for grace-hash spill (default 16)
    // spill triggered automatically by SpillManager when pool exceeds memory_limit_bytes
};

struct PhysicalNestedLoopJoin {
    JoinType        type;
    JoinCondition   condition;
    PhysicalNodePtr outer;
    PhysicalNodePtr inner;         // re-executed per outer row with correlated predicate
    std::string     correlated_column;  // column injected into inner scan as IN(values)
};

struct PhysicalBlockNestedLoopJoin {
    JoinType        type;
    JoinCondition   condition;
    PhysicalNodePtr outer;
    PhysicalNodePtr inner;
    uint32_t        block_size_rows;    // outer rows per block pass
};
```

These are added to the `PhysicalNode` variant alongside the existing scan/filter/project nodes.

### Join Optimizer Passes (added to the pipeline)

After `PredicatePushdown`, before `RequiredColumnCheck`, two new optimizer passes run:

**JoinOrderOptimizer** (new pass):
- Inspects both sides of every join
- If one side is `bounded` (required column has `=` or `IN` predicate) → that side becomes the build side of `PhysicalHashJoin`
- If both are bounded → smaller `IN` list size determines build side
- If both are unbounded → left-to-right order retained, emits a `PlanWarning` (not an error): `"JOIN on mldp.pv_metadata × mldp.time_series: both sides unbounded, hash table may be large"`

**CorrelatedPushOptimizer** (new pass):
- Checks if the join key column on the inner (probe) side is a `pushable_ops` column of that table (e.g. `mldp.time_series.pv`)
- If yes → switches from `PhysicalHashJoin` to `PhysicalNestedLoopJoin` and injects the outer-row join key values as an `IN(...)` predicate into the inner scan, which the inner backend can push natively
- This converts O(n×m) RPCs into O(n) RPCs where each inner RPC fetches exactly the rows matching the current outer batch
- Batch size is configurable via `--join-batch-size` (default: 100 outer rows per inner RPC)

### Join Column Disambiguation

When two tables share a column name (e.g. both have `pv`), `SELECT *` emits qualified names:
`left_alias.pv`, `right_alias.pv`. Unqualified column names in SELECT and WHERE that are
unambiguous (appear in only one table's schema) are auto-qualified by the Binder.
Ambiguous unqualified names → `BindError`: `"column 'pv' is ambiguous between 'ts' and 'meta'; qualify as ts.pv or meta.pv"`.

### `NULL` Semantics for LEFT JOIN

Missing rows on the right side in a `LEFT OUTER JOIN` produce `NULL` (empty string in
`QueryResult` row cells, with a per-column nullable flag set). `PhysicalFilter` predicates
that reference nullable columns use three-valued logic: `NULL` compared with anything yields
`false` (row excluded), matching standard SQL behaviour.

### Example Join Queries

```sql
-- Join pv_stats with pv_metadata to see archive stats alongside user tags (INNER)
SELECT ts.pv, ts.num_buckets, ts.last_timestamp, meta.tag
FROM mldp.pv_stats AS ts
INNER JOIN mldp.pv_metadata AS meta ON ts.pv = meta.pv
WHERE ts.pv IN ('QUAD:01:BDES', 'QUAD:02:BDES')

-- Left join: keep PVs even if no metadata record exists
SELECT ts.pv, ts.num_buckets, meta.description
FROM mldp.pv_stats AS ts
LEFT JOIN mldp.pv_metadata AS meta ON ts.pv = meta.pv
WHERE ts.pv IN ('QUAD:01:BDES', 'QUAD:02:BDES')

-- Correlated push example: stats for all PVs tagged 'magnet', then fetch time-series
-- Optimizer sees time_series.pv is pushable IN → converts to NestedLoopJoin:
--   outer scan: mldp.pv_metadata WHERE tag = 'magnet'  (fetches list of pv names)
--   inner scan: mldp.time_series  WHERE pv IN (<batch>) (batched RPC per outer block)
SELECT meta.pv, meta.description, ts.first_value, ts.last_timestamp
FROM mldp.pv_metadata AS meta
INNER JOIN mldp.time_series AS ts ON meta.pv = ts.pv
WHERE meta.tag = 'magnet'
  AND ts.time >= NOW-60s

-- Three-way join: config activations + config definition + pv stats
SELECT act.config_name, act.start_time, cfg.category, ts.num_buckets
FROM mldp.configuration_activation AS act
INNER JOIN mldp.configuration      AS cfg ON act.config_name = cfg.name
INNER JOIN mldp.pv_stats            AS ts  ON cfg.name = ts.pv
WHERE act.time >= '2025-01-01T00:00:00Z'
  AND act.time <= '2025-06-01T00:00:00Z'
```

### EXPLAIN with Joins

```
-- EXPLAIN for the correlated push example above:

PhysicalProject [meta.pv, meta.description, ts.first_value, ts.last_timestamp]
└─ PhysicalNestedLoopJoin INNER ON meta.pv = ts.pv
     algorithm:    correlated-push (batch_size=100)
     outer:
       PhysicalTableScan mldp.pv_metadata (alias: meta)
         backend:   MLDPAnnotationQueryClient
         pushable:  tag IN ('magnet')
         post-filter: (none)
     inner (re-executed per outer batch):
       PhysicalTableScan mldp.time_series (alias: ts)
         backend:   MLDPQueryClient
         pushable:  pv IN (<outer.meta.pv>)  ← correlated
                    time >= <epoch>           ← pushed constant
         post-filter: (none)
```

---

## Columnar Execution Model

### Internal Row Format: `arrow::RecordBatch`

All data inside the executor flows as `arrow::RecordBatch` — columnar, zero-copy, typed.
`QueryResult` (the public interface between `IQueryable` and the executor) is replaced by:

```cpp
// include/query/QueryResult.h
struct QueryResult {
    std::shared_ptr<arrow::RecordBatch> batch;      // columnar data
    std::string                          next_page_token;
};
```

`IQueryable::execute()` returns `QueryResult` with a typed `RecordBatch`.
Each column in the batch corresponds to a schema field with an Arrow type derived from
`ColumnType` at `tableSchema()` registration time:

| `ColumnType` | Arrow type |
|---|---|
| `STRING` | `arrow::utf8()` |
| `TIMESTAMP` | `arrow::timestamp(arrow::TimeUnit::SECOND, "UTC")` |
| `INT` | `arrow::int64()` |
| `BOOL` | `arrow::boolean()` |
| `DURATION_SECONDS` | `arrow::duration(arrow::TimeUnit::SECOND)` |

`PhysicalFilter`, `PhysicalProject`, `PhysicalLimit`, and all join executors operate directly
on `RecordBatch` using `arrow::compute` kernels (filter, take, cast) — no row-by-row string
conversion.

### Memory Pool and Budget

```cpp
// include/query/ExecutionContext.h
struct ExecutionContext {
    std::shared_ptr<arrow::MemoryPool>        pool;          // tracks all Arrow allocations
    std::shared_ptr<SpillManager>             spill;         // handles disk overflow
    uint64_t                                  memory_limit_bytes;
    uint32_t                                  join_batch_size;
    std::shared_ptr<arrow::fs::FileSystem>    spill_fs;      // injected FS backend
    std::string                               spill_dir;     // path within spill_fs
};
```

All executors receive an `ExecutionContext`. When `pool->bytes_allocated()` exceeds
`memory_limit_bytes`, the executor calls `spill->spillBatch(batch)` before allocating
the next batch.

---

## Spill Manager

Handles transparent spill of `RecordBatch` objects to `arrow::fs::FileSystem` when memory
is exhausted. Used by hash join build-side accumulation and blocked nested-loop join.

### Interface

```cpp
// include/query/SpillManager.h
class SpillManager {
public:
    // Write all batches in `batches` to a new spill file; returns opaque handle.
    SpillHandle spill(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                      const std::shared_ptr<arrow::Schema>& schema);

    // Stream back all batches from a spill handle (forward-only, sequential).
    // Deletes the spill file when the last batch is consumed.
    SpillReader read(SpillHandle handle);

    // Delete all spill files for the current query (called on query completion or error).
    void cleanup();

private:
    std::shared_ptr<arrow::fs::FileSystem> fs_;   // LocalFileSystem / MockFileSystem
    std::string                            dir_;   // SubTreeFileSystem root
    std::atomic<uint32_t>                  seq_{0};
};
```

### Spill File Format

Each spill file is an **Arrow IPC file** (`.arrow`):
- Written with `arrow::ipc::MakeFileWriter` → a single schema header + N record batches
- Read back with `arrow::ipc::OpenFile` → `arrow::ipc::RecordBatchFileReader`
- Memory-mapped on read (`arrow::io::MemoryMappedFile`) → zero-copy deser of column buffers

File naming: `spill_<query_id>_<seq>.arrow` under `spill_dir`.
`cleanup()` calls `fs_->DeleteFile(path)` for every handle not yet consumed.

### SpillHandle and SpillReader

```cpp
struct SpillHandle {
    std::string path;              // path within SpillManager::fs_
    std::shared_ptr<arrow::Schema> schema;
    uint64_t    bytes_written;
    uint32_t    num_batches;
};

class SpillReader {
public:
    bool nextBatch(std::shared_ptr<arrow::RecordBatch>& out);  // false = exhausted
    ~SpillReader();  // deletes spill file on destruction
};
```

---

## Revised Join Execution with Spill

### Hash Join with Spill-to-Disk

When the build side exceeds `memory_limit_bytes`, the hash join switches from a pure
in-memory hash table to a **partitioned spill join**:

```
Build phase:
  1. Accumulate build batches into hash table until memory limit hit.
  2. Partition remaining build batches by hash(join_key) % N_PARTITIONS.
  3. Spill each partition to disk via SpillManager.
  4. Keep partition 0 in memory (hot partition).

Probe phase:
  1. Probe in-memory partition 0 against all probe rows.
  2. For each remaining partition i:
       a. Load spill file i via SpillReader (memory-mapped)
       b. Build partition hash table in memory
       c. Probe against all probe rows
       d. SpillReader destructor deletes partition file
  3. LEFT JOIN: track unmatched build keys per partition using a secondary bitmap.
```

This degrades gracefully: small joins → pure memory hash join; large joins → partitioned
spill join. No hard failure, no aborted query. Memory usage stays bounded to
`memory_limit_bytes + largest_partition`.

`PhysicalHashJoin` gains a `spill_partitions` field (default 16, configurable via
`--spill-partitions`).

### Correlated Nested-Loop Join (unchanged, no spill needed)

Each inner batch is fetched, matched, and discarded immediately. Memory usage is
`O(batch_size)` — spill is irrelevant here.

### Block Nested-Loop Fallback with Spill

When `CorrelatedPushOptimizer` cannot promote (join key not pushable), and the build side
is unbounded, the executor:
1. Fetches the **entire outer side** into spill (batch by batch)
2. For each pass over the inner side: loads one block of outer rows from spill, probes inner
3. Emits `PlanWarning`: `"both sides unbounded — using block-nested-loop with spill; consider adding a WHERE predicate to bound one side"`

---

## Future: TCP Exposure via Arrow Flight SQL

When `--serve-flight <host:port>` is passed, the same engine is wrapped in an
`arrow::flight::FlightServerBase` subclass:

```
Client (DBeaver, pandas, ADBC, etc.)
    │  Arrow Flight SQL protocol (gRPC + IPC)
    ▼
FlightSQLServer
    ├─ GetFlightInfo(CommandStatementQuery) → parse + plan → FlightInfo (schema + ticket)
    ├─ DoGet(Ticket)                        → execute PhysicalPlan → stream RecordBatches
    └─ GetSchema(CommandStatementQuery)     → EXPLAIN → schema only
         │
         ▼ (same path as CLI query mode)
    QueryPlanner → PhysicalPlan → QueryExecutor → SpillManager → IQueryable → gRPC backends
```

All Arrow `RecordBatch` objects produced by the executor are streamed directly to the Flight
client without conversion — the columnar format is the wire format.

**Compatibility**: DBeaver, pandas (`adbc_driver_flight_sql`), JDBC/ODBC Flight drivers all
speak Arrow Flight SQL natively.

**No architectural change required**: `FlightSQLServer` wraps `QueryExecutor` directly.
The spill manager, physical plan, and all `IQueryable` backends are reused without modification.

Grammar extension for Flight mode:
```
-- Flight SQL clients send standard SQL; the parser needs no changes
-- SHOW TABLES → GetTables() Flight SQL call → QueryableFactory::registeredTables()
-- DESCRIBE    → GetSchema() Flight SQL call → IQueryable::tableSchema()
```

Future `--serve-flight` is a **Phase 7** item — not in scope for initial implementation,
but the design must not preclude it. Key invariant: `QueryExecutor::execute()` always
returns `arrow::RecordBatch` streams, never `vector<string>` rows.

---

## Architecture

### Components

```
mldp_pvxs_driver_main.cpp
  └─ dispatch "query" subcommand
       └─ src/cli/query/QuerySubcommand.cpp            ← arg: -c config.yaml + SQL string
            └─ include/query/ExecutionContext.h         ← MemoryPool + SpillManager + config
            └─ src/cli/query/SpillManager.h/.cpp        ← arrow::fs::FileSystem-backed spill
            └─ src/cli/query/QueryParser.cpp            ← tokeniser + recursive-descent parser
                 └─ QueryAST (SELECT/FROM/JOIN/WHERE/LIMIT nodes)
            └─ src/cli/query/QueryPlanner.cpp           ← multi-pass planner
                 ├─ planner/Binder.cpp                  ← resolve tables/columns/aliases
                 ├─ planner/TypeChecker.cpp             ← literal types + NOW folding
                 ├─ planner/LogicalPlanner.cpp          ← AST → LogicalPlan tree
                 ├─ planner/PredicatePushdown.cpp       ← pushable vs post-filter split
                 ├─ planner/ConstantFolding.cpp         ← dedup, literal folds
                 ├─ planner/ColumnPruning.cpp           ← projection hints
                 ├─ planner/JoinOrderOptimizer.cpp      ← build/probe side selection
                 ├─ planner/CorrelatedPushOptimizer.cpp ← hash→nested-loop promotion
                 ├─ planner/RequiredColumnCheck.cpp     ← required columns post-pushdown
                 └─ planner/PhysicalPlanner.cpp         ← LogicalPlan → PhysicalPlan
            └─ src/cli/query/QueryExecutor.cpp          ← walks PhysicalPlan; Arrow RecordBatch flow;
            │                                              spill via SpillManager when over memory limit
            └─ src/cli/query/QueryFormatter.cpp         ← RecordBatch → table / JSON / CSV / Arrow IPC
            └─ [future] src/cli/query/FlightSQLServer.cpp ← wraps QueryExecutor; Flight SQL wire protocol
```

### `IQueryable` Extension

Each `IQueryable` owns three responsibilities for every table it declares:
1. **Table names** — which virtual tables it handles (`kVirtualTables`)
2. **Schema** — column names, types, required flags for `DESCRIBE`
3. **Execution** — translate parsed AST predicates into its native backend call and return a `QueryResult`

The planner and executor become thin: planner validates the table exists, executor calls `execute()` and collects the result. No central predicate dispatch table — that logic lives exclusively inside each implementation.

```cpp
// include/query/IQueryable.h

enum class ColumnType { STRING, TIMESTAMP, DURATION_SECONDS, INT, BOOL };
enum class PredicateOp { EQ, NEQ, LT, LTE, GT, GTE, IN, PREFIX, CONTAINS, BETWEEN };

struct ColumnSchema {
    std::string            name;            // e.g. "pv", "time", "attr.<key>"
    ColumnType             type;
    bool                   required;        // must appear as pushable predicate or PlanError
    bool                   is_output;       // included in SELECT * expansion
    std::set<PredicateOp>  pushable_ops;    // ops backend handles natively
    std::set<PredicateOp>  filterable_ops;  // ops handled as in-memory post-filter
    std::string            notes;           // shown in DESCRIBE output
};

// Arrow Schema is derived from ColumnSchema at IQueryable registration time.
// ColumnType → arrow::DataType mapping is in include/query/ArrowTypeMap.h

struct QueryResult {
    std::shared_ptr<arrow::RecordBatch> batch;            // columnar Arrow batch
    std::string                          next_page_token; // empty = last page
};

class IQueryable {
public:
    virtual ~IQueryable() = default;

    // Set of virtual table names this instance handles.
    // Also exposed as static kVirtualTables for factory registration.
    virtual std::set<std::string_view> virtualTables() const = 0;

    // Column schema for a specific table (used by DESCRIBE and planner).
    virtual std::vector<ColumnSchema> tableSchema(std::string_view table_name) const = 0;

    // Translate pushable predicates for table_name into a backend call.
    // Returns an Arrow RecordBatch. Throws std::invalid_argument on bad predicates.
    // projection_hint: set of column names to include (empty = all); backends may ignore.
    virtual QueryResult execute(std::string_view                    table_name,
                                const std::vector<Predicate>&       pushable_predicates,
                                const std::set<std::string>&        projection_hint,
                                const ExecutionContext&              ctx) = 0;
};
```

Each concrete class adds:

```cpp
// MLDPQueryClient — owns mldp.time_series and mldp.pv_stats
static const std::set<std::string_view> kVirtualTables;
std::set<std::string_view> virtualTables() const override { return kVirtualTables; }
std::vector<ColumnSchema>  tableSchema(std::string_view table_name) const override;
QueryResult                execute(std::string_view table_name,
                                   const SelectStatement& stmt) override;
// in .cpp:
const std::set<std::string_view> MLDPQueryClient::kVirtualTables = {
    "mldp.time_series",
    "mldp.pv_stats",
};
// execute() switches on table_name:
//   "mldp.time_series" → extracts pv/time/timeout predicates → calls querySourcesData()
//   "mldp.pv_stats"    → extracts pv predicate              → calls querySourcesInfo()

// MLDPAnnotationQueryClient — owns pv_metadata, configuration, configuration_activation, active_configurations
static const std::set<std::string_view> kVirtualTables;
std::set<std::string_view> virtualTables() const override { return kVirtualTables; }
std::vector<ColumnSchema>  tableSchema(std::string_view table_name) const override;
QueryResult                execute(std::string_view table_name,
                                   const SelectStatement& stmt) override;
// in .cpp:
const std::set<std::string_view> MLDPAnnotationQueryClient::kVirtualTables = {
    "mldp.pv_metadata",
    "mldp.configuration",
    "mldp.configuration_activation",
    "mldp.active_configurations",
};
// execute() switches on table_name, translates predicates to proto criteria structs internally
```

### WHERE Predicate → Backend Mapping (owned by each implementation)

Each `execute()` impl validates predicates against its own column schema and translates them. The planner knows nothing about columns.

**`MLDPQueryClient::execute()` — internal mapping**

| Table | Predicate column | Backend param |
|---|---|---|
| `mldp.time_series` | `pv` (`IN`, `=`) | `source_names` set |
| `mldp.time_series` | `time` (`>=`) | `lookback_window = now - value` |
| `mldp.time_series` | `time` (`<=`) | `forward_window = value - now` |
| `mldp.time_series` | `timeout` (`=`) | `QuerySourcesDataOptions::timeout` |
| `mldp.time_series` | `rpc_deadline` (`=`) | `QuerySourcesDataOptions::rpc_deadline` |
| `mldp.pv_stats` | `pv` (`IN`, `=`) | `source_names` set |

**`MLDPAnnotationQueryClient::execute()` — internal mapping**

| Table | Predicate column | Backend param |
|---|---|---|
| `mldp.pv_metadata` | `pv` (`=`/`PREFIX`/`CONTAINS`/`IN`) | `PvNameCriterion` |
| `mldp.pv_metadata` | `alias` (`=`/`PREFIX`/`CONTAINS`/`IN`) | `AliasesCriterion` |
| `mldp.pv_metadata` | `tag` (`=`, `IN`) | `TagsCriterion` |
| `mldp.pv_metadata` | `attr.<key>` (`=`, `IN`) | `AttributesCriterion` |
| `mldp.configuration` | `name` (`=`/`PREFIX`/`CONTAINS`/`IN`) | `NameCriterion` |
| `mldp.configuration` | `category` (`=`, `IN`) | `CategoryCriterion` |
| `mldp.configuration` | `tag` (`=`, `IN`) | `TagsCriterion` |
| `mldp.configuration` | `attr.<key>` (`=`, `IN`) | `AttributesCriterion` |
| `mldp.configuration` | `parent` (`=`, `IN`) | `ParentCriterion` |
| `mldp.configuration_activation` | `time` (`=`) | `TimestampCriterion` |
| `mldp.configuration_activation` | `time` (`>=`/`<=`) | `TimeRangeCriterion` |
| `mldp.configuration_activation` | `config_name` (`=`, `IN`) | `ConfigurationNameCriterion` |
| `mldp.configuration_activation` | `activation_id` (`=`, `IN`) | `ClientActivationIdCriterion` |
| `mldp.configuration_activation` | `category` (`=`, `IN`) | `CategoryCriterion` |
| `mldp.configuration_activation` | `tag` (`=`, `IN`) | `TagsCriterion` |
| `mldp.configuration_activation` | `attr.<key>` (`=`, `IN`) | `AttributesCriterion` |
| `mldp.active_configurations` | `at` (`=`) | `GetActiveConfigurationsRequest::timestamp` |

Unknown column → `execute()` throws `std::invalid_argument` with message listing valid columns from `tableSchema()`.

### `QueryableFactory` Extension

```cpp
// Extended prepare<T>() — registers table names in addition to type_index
template <typename T>
void prepare(const config::Config& cfg, std::shared_ptr<metrics::Metrics> metrics = nullptr) {
    auto creator = [cfg, metrics]() -> IQueryableUPtr { return std::make_unique<T>(cfg, metrics); };
    std::unique_lock lock(mutex_);
    creators_[std::type_index(typeid(T))] = creator;
    // register each virtual table name from the static set
    for (const auto& table : T::kVirtualTables)
        table_creators_[std::string(table)] = creator;
}

// Lookup by table name — used by QueryExecutor
IQueryableUPtr createByTable(std::string_view table_name);

// List all registered table names — used by SHOW TABLES
std::set<std::string> registeredTables() const;
```

### `QueryPlanner` — Full Database-Style Multi-Pass Planner

The planner is the core intelligence of the query engine. It transforms a raw AST into an
optimised `PhysicalPlan` through a strict pipeline of passes. `QueryExecutor` only walks
the resulting physical plan — it contains no planning logic.

#### Pipeline Overview

```
AST (SelectStatement)
  │
  ├─ Pass 1: Binder       — resolve table name → IQueryable + ColumnSchema catalog
  ├─ Pass 2: TypeChecker  — validate predicate value types against column types;
  │                         evaluate NOW±duration → absolute epoch at plan time
  ├─ Pass 3: LogicalPlanner — build typed LogicalPlan tree
  │            LogicalScan(table, all_predicates)
  │              └─ LogicalFilter(non_pushable_predicates)    [if any]
  │                   └─ LogicalProject(selected_columns)
  │                        └─ LogicalLimit(n)                 [if LIMIT]
  ├─ Pass 4: Optimizer
  │            ├─ PredicatePushdown  — classify predicates as pushable vs post-filter
  │            ├─ ConstantFolding    — fold literal-vs-literal, collapse redundant predicates
  │            ├─ ColumnPruning      — drop columns not in SELECT or post-filter references
  │            └─ RequiredColumnCheck— error if a required column is absent from pushable set
  └─ Pass 5: PhysicalPlanner — LogicalPlan → PhysicalPlan
               PhysicalTableScan   ← IQueryable::execute() with pushable predicates only
               PhysicalFilter      ← in-memory predicate evaluation
               PhysicalProject     ← column selection / reorder
               PhysicalLimit       ← row cap
```

#### Extended `ColumnSchema`

`tableSchema()` now declares per-column pushability so the planner can classify predicates
without knowing anything about individual backends:

```cpp
enum class ColumnType { STRING, TIMESTAMP, DURATION_SECONDS, INT, BOOL };

enum class PredicateOp { EQ, NEQ, LT, LTE, GT, GTE, IN, PREFIX, CONTAINS, BETWEEN };

struct ColumnSchema {
    std::string            name;           // e.g. "pv", "time", "attr.<key>"
    ColumnType             type;
    bool                   required;       // must appear as pushable predicate or error
    bool                   is_output;      // included in SELECT * result
    std::set<PredicateOp>  pushable_ops;   // ops the backend handles natively
    std::set<PredicateOp>  filterable_ops; // ops plannable as in-memory post-filter
    std::string            notes;          // shown in DESCRIBE output
};
```

A predicate is **pushable** if its op is in `pushable_ops` → goes into `PhysicalTableScan`.
A predicate is **post-filterable** if its op is in `filterable_ops` → goes into `PhysicalFilter`.
A predicate on an unknown column or with an unsupported op → `BindError` at plan time, not at runtime.

#### Logical Plan Nodes

```cpp
// include/query/plan/LogicalPlan.h

struct LogicalScan   { std::string table_name; std::vector<Predicate> predicates; };
struct LogicalFilter { std::vector<Predicate> predicates; LogicalNodePtr child; };
struct LogicalProject{ std::vector<std::string> columns; LogicalNodePtr child; };  // empty = *
struct LogicalLimit  { uint32_t n; std::string page_token; LogicalNodePtr child; };

using LogicalNode = std::variant<LogicalScan, LogicalFilter, LogicalProject, LogicalLimit>;
using LogicalNodePtr = std::shared_ptr<LogicalNode>;
```

#### Physical Plan Nodes

```cpp
// include/query/plan/PhysicalPlan.h

struct PhysicalTableScan {
    std::string            table_name;
    IQueryable*            backend;          // non-owning; factory keeps ownership
    std::vector<Predicate> pushable;         // passed to IQueryable::execute()
};

struct PhysicalFilter {
    std::vector<Predicate> predicates;       // evaluated in-memory on QueryResult rows
    PhysicalNodePtr        child;
};

struct PhysicalProject {
    std::vector<std::string> columns;        // empty = pass-through all
    PhysicalNodePtr          child;
};

struct PhysicalLimit {
    uint32_t        n;
    std::string     page_token;
    PhysicalNodePtr child;
};

using PhysicalNode = std::variant<PhysicalTableScan, PhysicalFilter, PhysicalProject, PhysicalLimit>;
using PhysicalNodePtr = std::shared_ptr<PhysicalNode>;
```

#### Pass Detail: Binder

- Looks up `table_name` in `QueryableFactory` → error if not found (lists available tables)
- Loads `tableSchema(table_name)` → builds per-column lookup map
- Validates each predicate column exists in schema; expands `attr.<key>` patterns
- Validates each predicate operator is in `pushable_ops ∪ filterable_ops`; rejects unsupported ops with a typed error message: `"column 'pv' does not support operator '!='; supported: =, IN"`
- Binds `IN (SELECT ...)` with the same column resolution and `IN` capability validation as a literal list. A required column is covered only when `IN` is pushable.
- Validates `SELECT` column list against schema `is_output` columns; `*` expands to all output columns
- Checks `required` columns: if not covered by any predicate → `BindError` listing which required columns are missing

#### Pass Detail: TypeChecker

- Resolves `NOW` → `std::chrono::system_clock::now()` locked once per plan (deterministic across predicate pairs)
- Folds `NOW ± duration` → absolute `uint64_t epoch_seconds`
- Validates string literals against `ColumnType::TIMESTAMP` columns (ISO-8601 parse)
- Validates numeric literals against `ColumnType::INT` / `ColumnType::DURATION_SECONDS`
- Reports type mismatches: `"column 'time' expects TIMESTAMP, got string 'abc'"`

#### Pass Detail: Optimizer

**PredicatePushdown** (most important pass):
- Walks all predicates from `LogicalScan`
- Classifies each: `pushable_ops` membership → `PhysicalTableScan.pushable`; otherwise → `PhysicalFilter.predicates`
- Predicates on different columns with the same pushable op can be merged into a single backend criterion where the backend supports it (e.g. `tag = 'a' AND tag = 'b'` → two `TagsCriterion` entries, each AND-combined)
- For `IN (SELECT ...)`, the physical scan retains a typed child query. At execution, the child is materialized into ordinary `Predicate::values`; pushable membership goes into the backend request, while local-only membership filters fetched Arrow batches.

**ConstantFolding**:
- `pv = 'X' AND pv = 'X'` → single predicate (dedup)
- `LIMIT 0` → error
- Literal type errors caught here if TypeChecker missed edge cases

**ColumnPruning**:
- Builds the set of columns referenced by SELECT + PhysicalFilter predicates
- Retains the target column for local-only `IN (SELECT ...)` filtering
- Passes set to `PhysicalTableScan` so `execute()` can omit unrequested fields if the backend supports projection (optional hint — backends may ignore)

**RequiredColumnCheck**:
- Runs after PredicatePushdown; verifies every `required=true` column has at least one predicate in `PhysicalTableScan.pushable`
- A subquery predicate satisfies this check only when its target column supports pushable `IN`
- Fails with: `"table 'mldp.time_series' requires predicate on column 'pv'"`

#### EXPLAIN Support

```sql
EXPLAIN SELECT pv, num_buckets FROM mldp.pv_stats WHERE pv IN ('X', 'Y')
EXPLAIN SELECT * FROM mldp.pv_metadata WHERE tag = 'magnet' AND num_buckets > 5
```

`EXPLAIN` causes the planner to run all passes but the executor only prints the plan tree instead of executing it:

```
PhysicalProject [pv, num_buckets]
└─ PhysicalTableScan mldp.pv_stats
     backend:    MLDPQueryClient
     pushable:   pv IN ('X', 'Y')
     post-filter: (none)
     columns pruned to: [pv, num_buckets]

PhysicalProject [*]
└─ PhysicalFilter
│    predicates: num_buckets > 5   ← not pushable (backend has no filter for this)
└─ PhysicalTableScan mldp.pv_metadata
     backend:    MLDPAnnotationQueryClient
     pushable:   tag IN ('magnet')
     post-filter: num_buckets > 5
     note: num_buckets is a result field, filtered in memory after fetch
```

#### Query Stats

After execution, `QueryExecutor` returns a `QueryStats` struct alongside the result:

```cpp
struct QueryStats {
    std::chrono::milliseconds elapsed;
    uint64_t                  rows_from_backend;    // RecordBatch rows before post-filter
    uint64_t                  rows_returned;         // after post-filter + limit
    uint32_t                  rpc_calls;
    uint64_t                  bytes_spilled;         // bytes written to SpillManager
    uint32_t                  spill_files;           // spill files created
    uint64_t                  peak_memory_bytes;     // arrow::MemoryPool::max_memory()
    std::string               plan_summary;          // one-line physical plan description
};
```

Printed at end of output unless `--no-stats` flag is set:
```
-- 12 rows (42 from backend, 30 filtered) in 87ms | 1 RPC | 0 bytes spilled | 14 MB peak
   PhysicalTableScan(mldp.pv_metadata) → PhysicalFilter → PhysicalProject
```

#### Error Types (structured, not string-only)

```cpp
struct BindError   { std::string column; std::string message; };
struct TypeError   { std::string column; std::string expected; std::string got; };
struct PlanError   { std::string message; };  // required column missing, unknown table, etc.

using PlannerError = std::variant<BindError, TypeError, PlanError>;
```

Each error type formats a distinct, actionable user message pointing at the exact column or clause.

### Schema Introspection

```bash
mldp_pvxs_driver query "SHOW TABLES"
mldp_pvxs_driver query "DESCRIBE mldp.time_series"
mldp_pvxs_driver query "DESCRIBE mldp.pv_metadata"
```

`SHOW TABLES` → `QueryableFactory::registeredTables()`.
`DESCRIBE <table>` → `factory.createByTable(name)->tableSchema(name)` — schema served by the implementation itself.

---

## CLI Interface

```bash
# SQL string as positional arg
mldp_pvxs_driver query -c config.yaml "SELECT * FROM mldp.time_series WHERE pv IN ('MY:PV')"

# Minimal query-only config (no readers/writers/routing required)
mldp_pvxs_driver query -c query-only.yaml "SELECT * FROM mldp.pv_stats WHERE pv IN ('MY:PV')"

# Inline config override — queryable block only, no file needed
mldp_pvxs_driver query -c queryable.mldp[0].ingestion-url=localhost:50051 "SELECT * FROM mldp.pv_stats WHERE pv IN ('MY:PV')"

# From file
mldp_pvxs_driver query -c config.yaml --file query.sql

# Output formats
mldp_pvxs_driver query -c config.yaml "..." --format table   # default, human-readable
mldp_pvxs_driver query -c config.yaml "..." --format json    # JSON Lines to stdout
mldp_pvxs_driver query -c config.yaml "..." --format csv     # CSV to stdout

# Schema inspection (no config needed)
mldp_pvxs_driver query "SHOW TABLES"
mldp_pvxs_driver query "DESCRIBE mldp.pv_metadata"
```

### Query-Only Config

`QuerySubcommand` does **not** parse or validate readers, writers, routing, or metrics.
It extracts only the queryable connection config from the merged config tree — specifically
the keys consumed by `MLDPGrpcPoolConfig` and `MLDPGrpcAnnotationPoolConfig`.

A minimal query-only YAML needs only the `queryable:` root key — no readers, writers, or routing:

```yaml
# query-only.yaml — only queryable block required
queryable:
  mldp:
    - ingestion-url: "localhost:50051"
      query-url:     "localhost:50052"   # optional; falls back to ingestion-url
      min-conn: 1
      max-conn: 2
  mldp-annotation:
    - annotation-url: "localhost:50053"
      min-conn: 1
      max-conn: 2
```

Inline override also works (no file needed):

```bash
mldp_pvxs_driver query \
  -c queryable.mldp[0].ingestion-url=localhost:50051 \
  -c queryable.mldp-annotation[0].annotation-url=localhost:50053 \
  "SELECT * FROM mldp.pv_stats WHERE pv IN ('MY:PV')"
```

`QuerySubcommand` calls `loadMergedConfigSources()` (same as main driver) then constructs
only queryable instances via `QueryableFactory::prepare<T>(config)` reading from
`config.subConfig("queryable")` — never touching `MLDPPVXSController`. Config validation
skips readers/writers/routing keys — their absence is not an error in query mode.

---

## Operations & Parameters (retained for reference)

### mldp.time_series (`querySourcesData`)

| SQL Column | Maps To | Default |
|---|---|---|
| `pv` | `source_names` | required |
| `time >= NOW-Xs` | `lookback_window` | 30s |
| `time <= NOW+Xs` | `forward_window` | 1s |
| `timeout = N` | `QuerySourcesDataOptions::timeout` | 5000ms |
| `rpc_deadline = N` | `QuerySourcesDataOptions::rpc_deadline` | 5s |

### mldp.pv_stats (`querySourcesInfo`)

| SQL Column | Maps To |
|---|---|
| `pv` | `source_names` (required) |

Returns: `source_name`, `first_timestamp`, `last_timestamp`, `last_provider_id/name`,
`last_bucket_id`, `last_bucket_data_type`, `last_bucket_data_timestamps_type`,
`last_bucket_sample_period` (ns), `last_bucket_sample_count`, `num_buckets`.

### mldp.pv_metadata (`queryPvMetadata` / `getPvMetadata`)

| SQL Column | Operator(s) | Maps To |
|---|---|---|
| `pv` | `=`, `PREFIX`, `CONTAINS`, `IN` | `PvNameCriterion` |
| `alias` | `=`, `PREFIX`, `CONTAINS`, `IN` | `AliasesCriterion` |
| `tag` | `=`, `IN` | `TagsCriterion` |
| `attr.<key>` | `=`, `IN` | `AttributesCriterion` |

Pagination: `LIMIT N PAGE TOKEN 'tok'`.

### mldp.configuration (`queryConfigurations` / `getConfiguration`)

| SQL Column | Operator(s) | Maps To |
|---|---|---|
| `name` | `=`, `PREFIX`, `CONTAINS`, `IN` | `NameCriterion` |
| `category` | `=`, `IN` | `CategoryCriterion` |
| `tag` | `=`, `IN` | `TagsCriterion` |
| `attr.<key>` | `=`, `IN` | `AttributesCriterion` |
| `parent` | `=`, `IN` | `ParentCriterion` |

### mldp.configuration_activation (`queryConfigurationActivations`)

| SQL Column | Operator(s) | Maps To |
|---|---|---|
| `time` | `=` | `TimestampCriterion` (point-in-time) |
| `time` | `>=` / `<=` | `TimeRangeCriterion` |
| `config_name` | `=`, `IN` | `ConfigurationNameCriterion` |
| `activation_id` | `=`, `IN` | `ClientActivationIdCriterion` |
| `category` | `=`, `IN` | `CategoryCriterion` |
| `tag` | `=`, `IN` | `TagsCriterion` |
| `attr.<key>` | `=`, `IN` | `AttributesCriterion` |

### mldp.active_configurations (`getActiveConfigurations`)

| SQL Column | Operator | Maps To |
|---|---|---|
| `at` | `=` | `GetActiveConfigurationsRequest::timestamp` (required) |

---

## Not Yet Wired (proto exists, no C++ wrapper)

- `queryProviders` / `queryProviderStats` — future table `mldp.provider` / `mldp.provider_stats`
- `queryTable` — tabular format query (potential future `mldp.table` variant)
- `queryDataStream` / `queryDataBidiStream` — streaming (out of scope for SQL batch mode)
- All annotation write ops — out of scope (read-only query interface)

---

## Runtime Execution States

`QueryPlanner` produces an immutable `PhysicalPlan` tree.  At the start of
each `QueryExecutor::execute()` call, the executor recursively converts that
tree into a matching, query-local execution-state tree.  The state tree owns
its children with `std::unique_ptr`; states borrow the shared
`ExecutionContext` and mutable `QueryStats`.  It is discarded after the query
finishes, keeping planning free of live backend resources.

The runtime implementation is organized under `src/query/executor/` in three
families: scan states for table/catalog/derived scans and subquery
materialization; relational states for filter, project, sort, limit, and the
three joins; and statement states for SHOW, DESCRIBE, EXPLAIN, CREATE, and
DROP.  The factory uses exhaustive `PhysicalNodeVariant` dispatch and rejects
null plan nodes with an actionable error.

## Implementation Plan

| Phase | File | Summary |
|---|---|---|
| 0 | [phase-0-dependencies.md](phase-0-dependencies.md) | CMake Arrow/Flight linkage, Rocky Linux 9 GCC build |
| 1 | [phase-1-iqueryable-arrow.md](phase-1-iqueryable-arrow.md) | `IQueryable` contract, Arrow foundation, SpillManager, factory extension |
| 2 | [phase-2-parser.md](phase-2-parser.md) | Lexer, AST nodes, recursive-descent parser |
| 3 | [phase-3-planner-executor.md](phase-3-planner-executor.md) | Multi-pass planner, physical plan, executor |
| 3b | [phase-3b-join-plan-optimizer.md](phase-3b-join-plan-optimizer.md) | Join AST/logical/physical nodes, optimizer passes, join executor |
| 4 | [phase-4-queryable-impls.md](phase-4-queryable-impls.md) | `MLDPQueryClient` + `MLDPAnnotationQueryClient` execute() impls |
| 5 | [phase-5-output-cli.md](phase-5-output-cli.md) | QueryFormatter (table/json/csv/arrow), CLI arg wiring |
| 6 | [phase-6-tests.md](phase-6-tests.md) | Unit + integration test coverage |
| 7 | [phase-7-flight-sql.md](phase-7-flight-sql.md) | Arrow Flight SQL server (future, not initial scope) |
| 11 | [phase-11-unified-pull-executor.md](phase-11-unified-pull-executor.md) | One pull-based physical-operator framework for all query plans |
