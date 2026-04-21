# Extending Metrics: Per-Component Metric Classes

> **Related:** [Metrics Export Guide](metrics-export-guide.md) | [Architecture](architecture.md)

## Overview

The monolithic `Metrics` class covers readers, pool, controller, and bus domains.
When a component needs its own metric families — without polluting `Metrics` — it
subclasses the **`ExtendedMetrics`** hierarchy and registers directly into the shared
`prometheus::Registry`.

Because `PeriodicMetricsDumper` calls `metrics.registry()->Collect()`, every family
registered into that registry is exported automatically to both the Prometheus HTTP
endpoint and the JSONL file — no changes to `Metrics` or `PeriodicMetricsDumper`
required.

---

## Class Hierarchy

```
ExtendedMetrics                         include/metrics/ExtendedMetrics.h
└── WriterMetrics                       include/metrics/WriterMetrics.h
    └── HDF5WriterMetrics               include/writer/hdf5/HDF5WriterMetrics.h
```

| Class | Role |
|-------|------|
| `ExtendedMetrics` | Universal polymorphic root. No state. Pure virtual destructor only. |
| `WriterMetrics` | Marker base for all writer-domain metric classes. |
| `HDF5WriterMetrics` | Concrete HDF5 metrics: counters, gauges, histogram for the HDF5 writer. |

Add new domain roots (e.g. `ReaderMetrics`, `PoolMetrics`) by inheriting directly
from `ExtendedMetrics`; add concrete implementations under the domain root.

---

## How Metrics Flow to Export

```
HDF5WriterMetrics ──┐
                    │  register into shared registry
Metrics (core) ─────┤  (same prometheus::Registry instance)
                    │
                    ▼
         registry->Collect()
                    │
         ┌──────────┴──────────┐
         ▼                     ▼
  Prometheus HTTP         PeriodicMetricsDumper
  /metrics endpoint       → JSONL file
```

The shared registry is created inside `Metrics` and exposed via `Metrics::registry()`.
Any `ExtendedMetrics` subclass that receives a `prometheus::Registry&` reference at
construction time publishes into the same collection automatically.

---

## Step-by-Step: Creating a New Metrics Class

### 1. Choose the right base

| Component type | Inherit from |
|----------------|-------------|
| Writer variant | `WriterMetrics` |
| Anything else (reader, pool, …) | `ExtendedMetrics` directly |

### 2. Create the header

File: `include/<subsystem>/<Name>Metrics.h`

```cpp
#pragma once

#include <metrics/ExtendedMetrics.h>   // or WriterMetrics.h for writers

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <string>

namespace mldp_pvxs_driver::metrics {

class MyComponentMetrics : public ExtendedMetrics   // or WriterMetrics
{
public:
    MyComponentMetrics(prometheus::Registry& registry,
                       const std::string&    controller_name,
                       const std::string&    component_name);

    ~MyComponentMetrics() override = default;

    // --- public API ---
    void incrementEvents(double value = 1.0);
    void setQueueDepth(double value);
    void observeLatencyMs(double ms);

private:
    prometheus::Histogram::BucketBoundaries    latency_buckets_;
    prometheus::Family<prometheus::Counter>*   events_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     queue_depth_family_{nullptr};
    prometheus::Family<prometheus::Histogram>* latency_family_{nullptr};
};

} // namespace mldp_pvxs_driver::metrics
```

### 3. Create the implementation

File: `src/<subsystem>/<Name>Metrics.cpp`

```cpp
#include <<subsystem>/<Name>Metrics.h>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

using namespace mldp_pvxs_driver::metrics;

MyComponentMetrics::MyComponentMetrics(prometheus::Registry& registry,
                                       const std::string&    controller_name,
                                       const std::string&    component_name)
{
    const prometheus::Labels clabels{{"controller", controller_name},
                                     {"component",  component_name}};

    events_family_ = &prometheus::BuildCounter()
                          .Name("mldp_pvxs_driver_<component>_events_total")
                          .Help("Total events processed by <component>.")
                          .Labels(clabels)
                          .Register(registry);

    queue_depth_family_ = &prometheus::BuildGauge()
                               .Name("mldp_pvxs_driver_<component>_queue_depth")
                               .Help("Current queue depth for <component>.")
                               .Labels(clabels)
                               .Register(registry);

    latency_buckets_ = {0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0};

    latency_family_ = &prometheus::BuildHistogram()
                           .Name("mldp_pvxs_driver_<component>_latency_ms")
                           .Help("Processing latency for <component> (milliseconds).")
                           .Labels(clabels)
                           .Register(registry);
}

void MyComponentMetrics::incrementEvents(double value)
{
    events_family_->Add({}).Increment(value);
}

void MyComponentMetrics::setQueueDepth(double value)
{
    queue_depth_family_->Add({}).Set(value);
}

void MyComponentMetrics::observeLatencyMs(double ms)
{
    // Pass buckets on every Add() — Family<Histogram>::Add() is idempotent
    // for the same label set + same buckets, returning the same child.
    latency_family_->Add({}, latency_buckets_).Observe(ms);
}
```

> **Important — Histogram `Add()` rule:**  
> `Family<Histogram>::Add(labels, buckets)` requires buckets every call.
> Store `BucketBoundaries` as a member and pass it on every `observeX()` call.
> Calling `Add({})` without buckets will fail to compile (no default constructor).

### 4. Wire into the owning class

```cpp
// In the constructor that receives shared_ptr<Metrics>:
if (metrics)
{
    myMetrics_ = std::make_unique<MyComponentMetrics>(
        *metrics->registry(),
        metrics->controllerName(),
        config_.name);
}

// At each instrumented call site (null-check pattern):
if (myMetrics_) myMetrics_->incrementEvents();
```

### 5. Register the source file in CMakeLists.txt

```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/src/<subsystem>/<Name>Metrics.cpp"
```

If the component is HDF5-gated, place it inside the existing
`$<$<BOOL:${MLDP_PVXS_ENABLE_HDF5}>: … >` block.

---

## Label Conventions

All metric families must follow the project label conventions:

| Label | Scope | Value source |
|-------|-------|-------------|
| `controller` | constant per family | `Metrics::controllerName()` |
| `writer` / `component` | constant per family | instance config name |
| `source` | dynamic, per call | PV / source name at call time |

Dynamic labels are added at call time via `family->Add({{"source", pv_name}})`.
Constant labels are set once in the `prometheus::Build*().Labels(clabels)` chain.

---

## Naming Conventions

Metric names follow the pattern:

```
mldp_pvxs_driver_<domain>_<metric>_<unit_suffix>
```

| Suffix | When to use |
|--------|-------------|
| `_total` | Monotonically increasing counter |
| `_ms` | Histogram in milliseconds |
| `_seconds` | Histogram in seconds |
| `_bytes` | Counter or gauge of bytes |
| *(none)* | Gauge with self-describing name (e.g. `_queue_depth`) |

Examples from existing code:
- `mldp_pvxs_driver_hdf5_batches_written_total`
- `mldp_pvxs_driver_hdf5_write_latency_ms`
- `mldp_pvxs_driver_reader_queue_depth`

---

## Existing HDF5 Metrics Reference

`HDF5WriterMetrics` is the first concrete implementation. Constant labels:
`{controller=<name>, writer=<instance>}`.

| Metric | Type | Extra label | Instrumented at |
|--------|------|-------------|-----------------|
| `mldp_pvxs_driver_hdf5_batches_written_total` | Counter | — | `writerLoop()` after columnar/tabular write |
| `mldp_pvxs_driver_hdf5_rows_written_total` | Counter | `source` | `writerLoop()` per frame |
| `mldp_pvxs_driver_hdf5_bytes_written_total` | Counter | `source` | `writerLoop()` per frame |
| `mldp_pvxs_driver_hdf5_queue_depth` | Gauge | — | `writerLoop()` after queue drain |
| `mldp_pvxs_driver_hdf5_queue_drops_total` | Counter | — | `push()` on overflow |
| `mldp_pvxs_driver_hdf5_file_rotations_total` | Counter | `source` | `HDF5FilePool::acquire()` on rotation |
| `mldp_pvxs_driver_hdf5_write_latency_ms` | Histogram | — | `appendFrame()` / `flushTabularBuffer()` |

---

## Key Files

| File | Purpose |
|------|---------|
| `include/metrics/ExtendedMetrics.h` | Universal root base class |
| `include/metrics/WriterMetrics.h` | Writer-domain marker base |
| `include/metrics/Metrics.h` | Core metrics class; exposes `registry()` and `controllerName()` |
| `include/writer/hdf5/HDF5WriterMetrics.h` | Reference implementation |
| `src/writer/hdf5/HDF5WriterMetrics.cpp` | Registration + method bodies |
| `test/writer/hdf5/hdf5_writer_metrics_test.cpp` | Unit tests — counter/gauge/histogram assertions |
