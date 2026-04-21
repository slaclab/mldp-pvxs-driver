# TODO: Per-Writer Metrics Extension

## Problem

`Metrics` is a monolithic class hard-wired to reader/pool/bus/controller families.
`MLDPWriter` calls bus-family methods directly.
`HDF5Writer` accepts `shared_ptr<Metrics>` but **discards it** — zero HDF5 observability.

Different writers need different metrics. Adding all writer metrics to the single `Metrics` class creates a god object and is not scalable.

---

## Goal

Each writer implementation defines and owns its own metric families, registered into the **shared** Prometheus registry, using consistent label conventions (`controller`, `writer`, `source`).

---

## Design

### 1. `WriterMetrics` base interface

Create `include/metrics/WriterMetrics.h` — minimal abstract base:

```cpp
namespace mldp_pvxs_driver::metrics {

class WriterMetrics {
public:
    virtual ~WriterMetrics() = default;
};

} // namespace
```

Concrete subclasses carry writer-specific methods. The shared `prometheus::Registry` is injected so all writers publish to a single Prometheus endpoint (`metrics->registry()` already public).

---

### 2. `Metrics::controllerName()` accessor

`controller_name_` is currently private. Add:

```cpp
// include/metrics/Metrics.h
const std::string& controllerName() const;
```

Needed so writers can stamp the `controller` label without coupling to the config.

---

### 3. `HDF5WriterMetrics`

**New files:**
- `include/writer/hdf5/HDF5WriterMetrics.h`
- `src/writer/hdf5/HDF5WriterMetrics.cpp`

Constructor:
```cpp
HDF5WriterMetrics(prometheus::Registry& registry,
                  const std::string& controller_name,
                  const std::string& writer_name);
```

Constant labels on all families: `{controller=<name>, writer=<instance_name>}`.
Per-source dynamic label added at call time: `{source=<pv_name>}`.

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `mldp_pvxs_driver_hdf5_batches_written_total` | Counter | controller, writer | EventBatches written |
| `mldp_pvxs_driver_hdf5_rows_written_total` | Counter | controller, writer, source | Total rows appended |
| `mldp_pvxs_driver_hdf5_bytes_written_total` | Counter | controller, writer, source | Bytes written |
| `mldp_pvxs_driver_hdf5_queue_depth` | Gauge | controller, writer | Write queue depth |
| `mldp_pvxs_driver_hdf5_queue_drops_total` | Counter | controller, writer | Batches dropped on overflow |
| `mldp_pvxs_driver_hdf5_file_rotations_total` | Counter | controller, writer, source | File rotations |
| `mldp_pvxs_driver_hdf5_write_latency_ms` | Histogram | controller, writer | appendFrame / flush latency |

---

### 4. Wire into `HDF5Writer`

**`include/writer/hdf5/HDF5Writer.h`** — add private member:
```cpp
std::unique_ptr<HDF5WriterMetrics> writerMetrics_;
```

**`src/writer/hdf5/HDF5Writer.cpp`** — factory constructor:
```cpp
if (metrics) {
    writerMetrics_ = std::make_unique<HDF5WriterMetrics>(
        *metrics->registry(), metrics->controllerName(), config_.name);
}
```

Instrument call sites (null-check pattern `if (writerMetrics_) writerMetrics_->...`):

| Call site | Metric |
|-----------|--------|
| `appendFrame()` | `batches_written++`, `rows_written += N`, `bytes_written += B`, observe `write_latency_ms` |
| `push()` overflow eviction | `queue_drops++` |
| `writerLoop()` after dequeue | `queue_depth = queue_.size()` |
| `HDF5FilePool` rotation | `file_rotations++` (per source) |

For `file_rotations`: pass `HDF5WriterMetrics*` (non-owning) to `HDF5FilePool` via new setter:
```cpp
// include/writer/hdf5/HDF5FilePool.h
void setMetrics(HDF5WriterMetrics* metrics) noexcept;
```
Called from `HDF5Writer` constructor after pool creation.

---

### 5. `MLDPWriterMetrics` (Phase 2 / optional)

Existing `MLDPWriter` calls `Metrics` bus-family methods directly — works, but breaks the symmetry.
Follow-up: wrap bus-family calls into `MLDPWriterMetrics` for consistency.
Not blocking Phase 1.

---

## Files to Create

| File | Purpose |
|------|---------|
| `include/metrics/WriterMetrics.h` | Abstract base |
| `include/writer/hdf5/HDF5WriterMetrics.h` | HDF5 metric families declaration |
| `src/writer/hdf5/HDF5WriterMetrics.cpp` | Registration + method impl |
| `test/writer/hdf5/hdf5_writer_metrics_test.cpp` | Unit tests for HDF5 metrics |

## Files to Modify

| File | Change |
|------|--------|
| `include/metrics/Metrics.h` | Add `controllerName()` accessor |
| `src/metrics/Metrics.cpp` | Implement `controllerName()` |
| `include/writer/hdf5/HDF5Writer.h` | Add `writerMetrics_` member |
| `src/writer/hdf5/HDF5Writer.cpp` | Construct metrics; instrument call sites |
| `include/writer/hdf5/HDF5FilePool.h` | Add `setMetrics(HDF5WriterMetrics*)` |
| `src/writer/hdf5/HDF5FilePool.cpp` | Increment `file_rotations` on rotation |

---

## Label Conventions

- Constant per family: `{controller=<controller_name>, writer=<instance_name>}`
- Dynamic per call: `{source=<pv_name>}` — added via `.Add({{"source", src}})`, same pattern as existing reader metrics

---

## Verification

1. Build with `MLDP_PVXS_HDF5_ENABLED`.
2. Existing tests must pass: `hdf5_writer_config_test`, `hdf5_file_pool_test`, `mldppvxs_controller_hdf5_integration_test`.
3. New unit test: construct `HDF5WriterMetrics` with fresh registry, push batches, assert counter values.
4. Integration smoke: start driver with metrics endpoint, `curl localhost:9464/metrics` must show all `mldp_pvxs_driver_hdf5_*` families with correct labels.
