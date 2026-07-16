# `timestamp-clamp` Enricher

> **Back:** [Payload Enrichers](../enrichers.md) | **Related:** [Configuration Reference](../../guides/configuration.md#global-enrichers-and-writer-chains)

Clamps each frame timestamp's `nanoseconds` field to `999999999`, preventing invalid sub-second values from reaching downstream writers. Non-time-series variants are passed through unchanged.

## Configuration

```yaml
enrichers:
  clamp-ts:
    type: timestamp-clamp
```

No additional keys. The enricher takes no settings.

## Payload Scope

Operates on **time-series** payloads only. All other variants are accepted unchanged.

## Behavior Details

- Applies `min(nanoseconds, 999999999)` to every timestamp in every frame of the batch.
- The `seconds` field is never modified.
- Returns `true` (accept) always; it never drops a batch.
- Holds no state; safe to share across writer chains.

## When to Use

Some EPICS IOC clocks or network-synchronized timestamps produce nanosecond values above `999999999` due to firmware bugs or rounding. MongoDB and HDF5 writers expect the nanosecond component to be in the valid range `0..999999999`. Attach `timestamp-clamp` before any writer that rejects or misinterprets over-range sub-second values.

## Example

```yaml
enrichers:
  ts-fix:
    type: timestamp-clamp

writer:
  mldp:
    - name: mldp_main
      enrichers: [ts-fix]
```
