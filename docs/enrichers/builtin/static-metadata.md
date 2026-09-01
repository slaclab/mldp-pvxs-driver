# `static-metadata` Enricher

> **Back:** [Payload Enrichers](../enrichers.md) | **Related:** [Configuration Reference](../../guides/configuration.md#global-enrichers-and-writer-chains)

Merges a fixed set of string key/value pairs into every batch's `metadata` map. Configured values overwrite existing metadata keys; keys absent from the configuration are untouched.

## Configuration

```yaml
enrichers:
  run-context:
    type: static-metadata
    metadata:
      experiment_id: run-42
      facility: LCLS
      operator: jane
```

| Key | Required | Type | Description |
|---|---|---|---|
| `metadata` | Yes | mapping of strings | Key/value pairs merged into every batch. Values must be scalars convertible to string. |

Startup fails if `metadata` is missing or is not a mapping.

## Payload Scope

Applies to **all four** `EventBatch` payload variants: time series, source metadata, configuration, and configuration activation. Use it whenever metadata must accompany any batch type from a reader.

## Behavior Details

- Keys in `metadata` overwrite matching keys in the batch; other keys are untouched.
- The enricher holds no per-batch state; it is safe to share across multiple writer chains.
- Returns `true` (accept) always; it never drops a batch.

## Example: tagging all writers with an experiment identifier

```yaml
enrichers:
  experiment-tag:
    type: static-metadata
    metadata:
      experiment_id: lcls-2025-run3
      site: slac

writer:
  mldp:
    - name: mldp_main
      enrichers: [experiment-tag]
  hdf5:
    - name: hdf5_raw
      enrichers: [experiment-tag]
```

One shared instance stamps the same keys into batches routed to both writers.
