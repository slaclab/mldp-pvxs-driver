# `shard-slot` Enricher

> **Back:** [Payload Enrichers](../enrichers.md) | **Related:** [Configuration Reference](../../guides/configuration.md#global-enrichers-and-writer-chains)

Assigns a stable, process-lifetime `shardSlot` attribute to each first-seen column name, distributing columns across MongoDB shard ranges for balanced data placement. Non-time-series variants are passed through unchanged.

## Configuration

```yaml
enrichers:
  sharding:
    type: shard-slot
    num-shards: 6     # optional; default 6
```

| Key | Required | Type | Default | Description |
|---|---|---|---|---|
| `num-shards` | No | integer | `6` | Number of shard buckets. Must be in the range `1..65536`. |

Startup fails if `num-shards` is outside `1..65536`.

## Payload Scope

Operates on **time-series** payloads only. All other variants are accepted unchanged.

## Behavior Details

**Assignment algorithm:**

1. On first encounter of a column name, assign it to shard `N % num-shards` (round-robin counter, monotonically increasing).
2. Pick a random slot within that shard's range using a uniform distribution over `[lower, upper]`, where the range is `[65536*shard/num-shards, 65536*(shard+1)/num-shards - 1]`.
3. Format the slot as a zero-padded five-digit decimal string and write it to `column.metadata["shardSlot"]`.
4. On subsequent encounters of the same column name, reuse the stored slot value. Skip columns that already carry a `shardSlot` attribute.

**Stability constraints:**

- Mappings are in-memory only. A process restart remaps all columns.
- Changing `num-shards` between restarts can split a PV's historical data and new data across different MongoDB shards. Only change `num-shards` when starting a fresh data collection.
- HDF5 BSAS Gen1 readers no longer configure `num-shards` or stamp this attribute; attach `shard-slot` explicitly to writers that need it.

## Example

```yaml
enrichers:
  sharding:
    type: shard-slot
    num-shards: 8

writer:
  mldp:
    - name: mldp_main
      enrichers: [sharding]
```

With `num-shards: 8`, each shard covers `65536 / 8 = 8192` slots. The first-seen column gets a random slot in `[0, 8191]`, the second in `[8192, 16383]`, and so on, cycling every 8 columns.

## Sharing Across Writers

A single `shard-slot` instance shared across multiple writers assigns consistent slots for the same column names across all writers, since the slot map is per-instance. Use one shared definition unless different writers need independent shard assignments.
