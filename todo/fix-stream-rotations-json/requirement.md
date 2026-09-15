# Fix stream_rotations_total JSON Serialization

## Priority

P0 — every metrics snapshot is unparseable at the stream_rotations_total line

## Problem

`mldp_pvxs_driver_writer_stream_rotations_total` emits invalid JSON in every snapshot:

```json
"mldp_pvxs_driver_writer_stream_rotations_total": [
  {"controller": "gen-1-ingestion-driver", "reason": "stream age exceeded (idle)",
   "writer": "mldp_gen1_ingestion", "value": age},
  {"controller": "gen-1-ingestion-driver", "reason": "threshold reached",
   "writer": "mldp_gen1_ingestion", "value": reached",writer="mldp_gen1_ingestion"}}
]
```

Two defects visible:
1. `"value": age` — bare identifier `age` instead of a number (missing `std::to_string` or equivalent)
2. Second entry has `"value": reached",writer="mldp_gen1_ingestion"}}` — truncated + injected key fragment, likely string concatenation bug in the serializer

The entire containing JSON object is corrupt, so all downstream consumers (Prometheus scraper, metrics tooling, analysis scripts) fail to parse the snapshot.

## Root cause area

Metrics JSON serialization code for counter families. Look for where `stream_rotations_total` labels are assembled into JSON — variable names are being emitted unquoted/unformatted instead of their numeric values.

Likely in `src/metrics/Metrics.cpp` or the JSON serializer invoked from the metrics export path.

## Fix

1. Locate the `stream_rotations_total` serialization site.
2. Ensure counter values are rendered as JSON numbers (`std::to_string(value)` or `nlohmann::json` / equivalent).
3. Ensure label key-value pairs use proper JSON string quoting — no string concatenation that can produce key injection.
4. Add a unit test or parsing assertion that verifies the full metrics JSON snapshot round-trips through `nlohmann::json::parse` without exception.

## Acceptance criteria

- `metrics.jsonl` output parses cleanly with `python3 -c "import json; [json.loads(l) for l in open('metrics.jsonl')]"` — zero exceptions.
- `stream_rotations_total` entries have numeric `value` fields.
- Both `reason` label variants (`stream age exceeded (idle)`, `threshold reached`) appear with correct integer counts.
