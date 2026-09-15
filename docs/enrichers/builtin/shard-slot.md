# `shard-slot` Enricher

> **Back:** [Payload Enrichers](../enrichers.md) | **Related:** [Configuration Reference](../../guides/configuration.md#global-enrichers-and-writer-chains)

Assigns a stable `shardSlot` attribute to each first-seen column name, distributing columns across MongoDB shard ranges for balanced data placement. Assignments are persisted to a SQLite database so they survive process restarts and can be shared across multiple driver instances pointing at the same file. Non-time-series variants are passed through unchanged.

## Configuration

```yaml
enrichers:
  sharding:
    type: shard-slot
    num-shards: 6                          # optional; default 6
    db-path: /var/lib/mldp/shard_slots.db  # required
```

| Key | Required | Type | Default | Description |
|---|---|---|---|---|
| `db-path` | **Yes** | string | — | Path to the SQLite database file. Created on first run. Multiple driver instances may point at the same file; WAL mode ensures safe concurrent access. |
| `num-shards` | No | integer | `6` | Number of shard buckets. Must be in the range `1..65536`. |

Startup fails if `db-path` is missing or empty, if the file cannot be opened or created, or if `num-shards` is outside `1..65536`.

## Payload Scope

Operates on **time-series** payloads only. All other variants are accepted unchanged.

## Behavior Details

**Assignment algorithm:**

1. On startup, load all existing `(column_name → slot)` rows from the database into the in-memory cache.
2. On first encounter of a column name not yet in the cache, assign it to shard `N % num-shards` (round-robin counter, monotonically increasing across new assignments in this process run).
3. Pick a random slot within that shard's range using a uniform distribution over `[lower, upper]`, where the range is `[65536*shard/num-shards, 65536*(shard+1)/num-shards - 1]`.
4. Write the assignment to the database via `INSERT OR IGNORE`, then re-read the stored value. If another instance already inserted this column, its value wins and the local cache is updated to match.
5. Format the slot as a zero-padded five-digit decimal string and write it to `column.metadata["shardSlot"]`.
6. On subsequent encounters of the same column name, reuse the cached slot. Skip columns that already carry a `shardSlot` attribute.

**Stability guarantees:**

- Assignments survive process restarts: the database is the source of truth.
- Multiple driver instances sharing the same `db-path` converge to the same slot for each column. The first instance to write a column wins; later instances adopt its value.
- Changing `num-shards` after data has been written splits a PV's historical and new data across different MongoDB shards. Only change `num-shards` when starting a fresh data collection with an empty database.
- HDF5 BSAS Gen1 readers do not stamp this attribute; attach `shard-slot` explicitly to the writers that need it.

**Concurrency model:**

| Scenario | How it is handled |
|---|---|
| Multiple writer chains in one process | Per-instance mutex in `IPayloadEnricher::run()` serialises calls; no extra locking needed. |
| Multiple driver processes, same `db-path` | SQLite WAL mode allows concurrent readers and one writer. `INSERT OR IGNORE` + re-read ensures convergence. A 5-second busy timeout prevents immediate failure under transient lock contention. |
| Same `db-path`, different `num-shards` | No protection — using different `num-shards` values across instances on the same database is unsupported and will produce inconsistent slot ranges. |

## Shared Database Example

Two driver instances processing different readers but writing to the same MongoDB cluster:

```yaml
# Instance A and Instance B — identical enricher block, same db-path
enrichers:
  sharding:
    type: shard-slot
    num-shards: 6
    db-path: /shared/nfs/mldp/shard_slots.db

writer:
  mldp:
    - name: mldp_main
      enrichers: [sharding]
```

Both instances load existing assignments at startup and insert new ones atomically. A column first seen by instance A gets the same slot on instance B once its assignment is written.

## Single-Instance Example

```yaml
enrichers:
  sharding:
    type: shard-slot
    num-shards: 8
    db-path: /var/lib/mldp/shard_slots.db

writer:
  mldp:
    - name: mldp_main
      enrichers: [sharding]
```

With `num-shards: 8`, each shard covers `65536 / 8 = 8192` slots. The first-seen column gets a random slot in `[0, 8191]`, the second in `[8192, 16383]`, and so on, cycling every 8 columns.

## SQLite Database Layout

The enricher creates one table on first run:

```sql
CREATE TABLE IF NOT EXISTS shard_slots (
    source_name TEXT PRIMARY KEY,
    slot        INTEGER NOT NULL
);
```

The database uses WAL journal mode (`PRAGMA journal_mode=WAL`) and a 5-second busy timeout. No migrations are needed; the schema is stable.

## Sharing Across Writers

A single `shard-slot` instance shared across multiple writers within one process assigns consistent slots for the same column names across all writers, since the slot map is per-instance. Use one shared definition unless different writers need independent shard assignments.
