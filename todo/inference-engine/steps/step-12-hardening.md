# Step 12 — Hardening: Metrics, Back-pressure, Config Validation, Cycle Detection

## Goal

Production-harden the processor infrastructure. No new algorithm types.
Each sub-task is independent — implement in order but each compiles and tests alone.

## Depends On

Steps 01–11.

---

## 12a — Processor Metrics

### What to add

In `ChannelProcessor::fireCompute()`:
```cpp
// Measure compute latency:
auto t0 = std::chrono::steady_clock::now();
outputs = algorithm_->compute(snap);
auto t1 = std::chrono::steady_clock::now();
// Record to metrics (histogram or gauge):
//   "processor_compute_latency_us" label: processor_name
//   "processor_fire_count"          label: processor_name
//   "processor_buffer_depth"        label: processor_name  (slots_.size() or fresh_.size())
```

Use existing `metrics::Metrics` pattern (look at how writers record latency).

### Files to modify

- `src/processor/ChannelProcessor.cpp`

### Verification

Run existing tests + check metrics object is populated (add 1 assertion to `ChannelProcessorTest`
that metrics counter increments after compute).

---

## 12b — Back-pressure: Max Buffer Depth

### What to add

`MLDPChannelProcessorConfig`: add optional `max-buffer-depth` key (default: 0 = unlimited).
`InputBuffer`: add `maxDepth` param. In `ingest()`: if `slots_[source].timestamps.size() >= maxDepth`,
drop oldest timestamp + column samples (keep `[1..]` slice).

### Config

```yaml
- type: linear-transform
  name: my-proc
  sources: [SRC:A]
  max-buffer-depth: 100   # optional, drop oldest when exceeded
  ...
```

### Tests to add (append to `InputBufferTest.cpp`)

| Test name | Scenario |
|---|---|
| `BackPressure_DropsOldest` | depth=2, ingest 3 values → only 2 newest in slot |
| `BackPressure_Unlimited` | depth=0, ingest 100 values → all kept |

---

## 12c — Config Validation: output-source Collision

### What to add

In `MLDPPVXSController::start()`, after processors and readers are constructed:
```cpp
// Collect all real reader root_sources (from readerEntries / route table known_readers).
// Collect all processor outputSourceNames().
// If any output source name equals a real reader name: throw std::runtime_error.
```

Real reader names are the `name()` of each reader. Output source names are virtual PV strings.
Check: for each `p->outputSourceNames()`, none should equal `r->name()` for any real reader.
(Full root_source collision would require readers to advertise their root_sources — too expensive for v1.
Check processor name vs reader name as a first guard.)

### Tests to add (append to controller integration test)

| Test name | Scenario |
|---|---|
| `OutputSourceCollidesWithReaderName_Throws` | processor output-source == reader name → start() throws |

---

## 12d — Circular Chain Detection

### What to add

In `MLDPPVXSController::start()`, after all processors are constructed:

Build a directed graph: edge `A → B` when processor B's `inputSourceNames()` contains
any of processor A's `outputSourceNames()`.

Run DFS cycle detection. If cycle found: throw `std::runtime_error` with cycle description.

**Example**: Processor A emits `VIRTUAL:X`, Processor B sources `[VIRTUAL:X]` and emits `VIRTUAL:Y`,
Processor A sources `[VIRTUAL:Y]` → cycle A→B→A → throw.

### Tests to add (append to controller integration test)

| Test name | Scenario |
|---|---|
| `ProcessorChain_NoCycle_OK` | A→B linear chain → start() succeeds |
| `ProcessorChain_Cycle_Throws` | A→B→A cycle → start() throws |

---

## Files to Modify

- `src/processor/ChannelProcessor.cpp` (12a)
- `include/processor/InputBuffer.h` + `src/processor/InputBuffer.cpp` (12b)
- `include/processor/MLDPChannelProcessorConfig.h` + `src/processor/MLDPChannelProcessorConfig.cpp` (12b)
- `src/controller/MLDPPVXSController.cpp` (12c, 12d)

---

## CMake Changes

None.

---

## Verification

```bash
cmake --build build_test --target mldp_pvxs_driver_test 2>&1 | tail -5
cd build_test && ctest -V
```

All tests must pass including new ones added in this step.

## Done Criteria

- Metrics increment after each compute (12a).
- Buffer depth limiting works: old samples dropped (12b).
- Output-source collision detected and thrown at startup (12c).
- Circular chain detected and thrown at startup (12d).
- All existing tests pass.
