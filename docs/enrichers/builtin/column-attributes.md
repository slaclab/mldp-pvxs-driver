# `column-attributes` Enricher

> **Back:** [Payload Enrichers](../enrichers.md) | **Related:** [Configuration Reference](../../guides/configuration.md#global-enrichers-and-writer-chains)

Applies a fixed set of string key/value attributes to every `DataColumn` whose name matches a glob pattern. Non-time-series payload variants are passed through unchanged.

## Configuration

```yaml
enrichers:
  bpm-attrs:
    type: column-attributes
    column-pattern: "BPM:*"
    attributes:
      device_class: bpm
      units: mm
```

| Key | Required | Type | Description |
|---|---|---|---|
| `column-pattern` | Yes | string | POSIX `fnmatch` glob applied to each column's `name`. |
| `attributes` | Yes | mapping of strings | Key/value pairs merged into matching columns' `metadata`. |

Startup fails if either key is missing or `attributes` is not a mapping.

## Payload Scope

Operates on **time-series** payloads only. All other variants (`source-metadata`, `configuration`, `configuration-activation`) are accepted unchanged.

## Behavior Details

- Pattern matching uses POSIX `fnmatch(3)` with no flags; `*` matches any sequence of characters excluding `/`. Use `?` for single character.
- Configured attribute values overwrite matching keys in a column's metadata; other column metadata keys are untouched.
- All columns in all frames across the batch are visited on every call.
- Returns `true` (accept) always; it never drops a batch.

## Glob Pattern Examples

| Pattern | Matches |
|---|---|
| `BPM:*` | All columns starting with `BPM:` |
| `*:X` | All columns ending with `:X` |
| `BPM:?:X` | Columns like `BPM:A:X`, `BPM:1:X` |
| `*` | All columns |

## Example: adding units to beam-position columns

```yaml
enrichers:
  bpm-x-units:
    type: column-attributes
    column-pattern: "BPM:*:X"
    attributes:
      units: mm
      sensor: horizontal

writer:
  mldp:
    - name: mldp_main
      enrichers: [bpm-x-units]
```
