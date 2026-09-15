# Add Configurable Bounded Controller Queue + Drop Counter (P3)

## Priority

P3 — unbounded queue is safe today but becomes an OOM risk if gRPC stalls

## Problem

`MLDPWriter::push()` enqueues one `QueueItem` per frame into per-worker
`channels_[idx]->items` (a `std::deque`). There is no maximum depth check.

During normal bursts the queue peaks at 665 items and drains in < 10 s.
However if a gRPC stream stalls (network partition, server overload,
TLS renegotiation), the worker blocks inside `writer->Write(request)` and
its channel queue grows without bound until:

- Process RSS hits system OOM killer threshold, or
- Upstream reader drops events because the bus `push()` call stacks up
  on `queuedItems_` without any backpressure signal.

Neither case is handled today.

## Fix

### 1. Configurable max queue depth

Add `queue-max-depth` (integer, default: 0 = unlimited, ≥ 1 = bounded)
to `MLDPWriterConfig`.

In `push()`, before enqueuing a frame, check:

```cpp
if (config_.queueMaxDepth > 0 &&
    queuedItems_.load(std::memory_order_relaxed) >= config_.queueMaxDepth)
{
    metric_call(metrics_, [&](auto& m) {
        m.incrementWriterDrops(1.0, {{"writer", config_.name}, {"source", rootSourceName}});
    });
    continue;  // drop this frame, keep going for remaining frames
}
```

### 2. `writer_drops_total` counter metric

Add to `Metrics`:

```cpp
void incrementWriterDrops(double value = 1.0, prometheus::Labels tags = {});
```

Prometheus metric: `mldp_pvxs_driver_writer_drops_total` (counter).
Labels: `controller`, `writer`, `source`.

Document: a non-zero value here means the queue depth limit was reached —
operator must increase `pool-size` or lower source update rate.

### 3. Log warning on first drop per writer per run

Emit a `warnf` when `drop_count` transitions from 0 to 1 to avoid log spam:

```
MLDPWriter[mldp_gen1_ingestion]: queue full (depth=N), dropping frame for source X.
Increase pool-size or reduce source update rate.
```

## Configuration YAML

```yaml
writer:
  mldp:
    - name: mldp_gen1_ingestion
      pool-size: 4          # see configurable-grpc-pool-size todo
      queue-max-depth: 2000  # 0 = unlimited (default)
```

## Acceptance criteria

- `queue-max-depth: 100` configured → queue never exceeds 100 items in metrics.
- Dropped frames increment `mldp_pvxs_driver_writer_drops_total`.
- Warning logged exactly once when first drop occurs (not per drop).
- `queue-max-depth: 0` (default) → existing behavior preserved (no drops, no bound).
- Config parse test for new field; existing integration tests pass unchanged.
