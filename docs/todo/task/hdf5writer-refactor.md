# HDF5Writer Refactor & Bug Fix Plan

Source: deep review of `src/writer/hdf5/HDF5Writer.cpp`

## Test Coverage Status

Regression tests added to `test/writer/hdf5/hdf5_writer_test.cpp`:

| Item | Test name | Status |
|------|-----------|--------|
| 2 — mergeBytesWritten_ tabular | `MergeTabularSizeRotationFiresAfterThreshold` | ✅ added — will FAIL until fixed |
| 3 — queue depth metric | `QueueDepthMetricPathExercised` | ✅ added — exercises metric wire-up |
| 6 — TOCTOU rotation | — | ⚠️ not unit-testable reliably |
| 7 — silent tabular data drop | `TabularMidRoundTimestampChangeDoesNotDropData` | ✅ added — will FAIL until fixed |
| 8 — roundFirstTs stale | `TabularRoundFirstTsResetAfterFlush` | ✅ added — may FAIL until fixed |
| 11 — warnedUnknown unbounded | `TabularUnknownColumnNoDuplicatesInWarnSet` | ✅ added — smoke test; TODO tighten after cap added |
| 1,4,5,9,10,12,13 | — | structural/refactor — no runtime test needed |

---

## Priority Order

Tasks grouped by severity. Implement in order — structural fixes first, then correctness, then perf/minor.

---

## TASK 1 — Deduplicate column-write visit lambda [MAJOR]

**Files**: `HDF5Writer.cpp`, `HDF5Writer.h`

**Problem**: ~80-line `std::visit` lambda duplicated between `appendFrame()` (lines 657–752) and `appendFrameMerge()` (lines 1280–1366). Every type arm copy-pasted. Bug fix requires double change.

**Fix**: Extract private templated helper:
```cpp
// HDF5Writer.h
template <typename EnsureFn1D, typename EnsureFn2D, typename PostWriteFn>
void writeColumns(const util::bus::DataBatch& batch,
                  EnsureFn1D&&  ensure1D,
                  EnsureFn2D&&  ensure2D,
                  PostWriteFn&& postWrite);
```
- `appendFrame()` passes lambdas that capture `file`; `postWrite` is no-op.
- `appendFrameMerge()` passes lambdas that capture `*mergeFile_`; `postWrite` increments `mergeBytesWritten_`.
- Single source of truth for all type arms.

**Acceptance**: `appendFrame` and `appendFrameMerge` column sections each collapse to one `writeColumns(...)` call.

---

## TASK 2 — Fix `mergeBytesWritten_` not updated in tabular merge path [MAJOR / FUNCTIONAL BUG]

**Files**: `HDF5Writer.cpp`

**Problem**: `flushTabularBufferMerge()` (lines 1371–1390) calls `flushTabularBuffer()` which writes to `*mergeFile_` but never touches `mergeBytesWritten_`. Size-based rotation never fires for tabular merge workloads → unbounded file growth.

**Fix**: After `flushTabularBuffer()` returns in `flushTabularBufferMerge()`, compute bytes written (nRows × actual column sizes) and add to `mergeBytesWritten_`. Alternatively, pass a `bytesWrittenOut` out-param to `flushTabularBuffer()`.

**Acceptance**: Write 1M rows tabular to merge path, verify `checkMergeRotation()` triggers before file exceeds `maxFileSizeMB`.

---

## TASK 3 — Fix queue-depth metric always 0 [MAJOR]

**Files**: `HDF5Writer.cpp` line 284

**Problem**:
```cpp
writerMetrics_->setQueueDepth(static_cast<double>(queue_.size()));
```
`queue_` was just swapped out — always 0. Metric useless.

**Fix**: Capture depth before drain:
```cpp
// inside the lock, before swap:
const auto queueDepthAtDrain = static_cast<double>(queue_.size());
drained.swap(queue_);
// after lock released:
if (writerMetrics_)
    writerMetrics_->setQueueDepth(queueDepthAtDrain);
```

**Acceptance**: Queue depth metric reflects batch count seen at each drain wake-up.

---

## TASK 4 — Fix `append1D`/`append2D` redundant `getSpace()` call [CRITICAL / CLARITY]

**Files**: `HDF5Writer.cpp` lines 481–511 (anonymous namespace)

**Problem**: First `ds.getSpace()` (line 484) is temporary, result discarded immediately after `getSimpleExtentDims`. Second `getSpace()` (line 487) reads post-extend dims. Pre-extend `curDims` from first call is never used — `offset` computed from post-extend dims. Misleading; one wasted `H5Sopen/close` round-trip per write.

**Fix**:
```cpp
template <typename CType>
void append1D(H5::DataSet& ds, const H5::DataType& h5type, const CType* data, hsize_t n)
{
    hsize_t preDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
    ds.getSpace().getSimpleExtentDims(preDims, maxDims);  // save pre-extend size
    const hsize_t newSize = preDims[0] + n;
    ds.extend(&newSize);
    H5::DataSpace fspace = ds.getSpace();
    hsize_t offset[1] = {preDims[0]};                    // use saved pre-extend offset
    hsize_t count[1]  = {n};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(1, count);
    ds.write(data, h5type, mspace, fspace);
}
```
Apply same pattern to `append2D`.

**Acceptance**: Logically equivalent, no second `getSimpleExtentDims` call after extend.

---

## TASK 5 — Fix VL string pointer fragility [CRITICAL]

**Files**: `HDF5Writer.cpp` lines 695–700 and 1313–1319

**Problem**: `std::vector<const char*>` holds raw `c_str()` pointers into `vals` strings. Safe today (sync path). Fragile — any future async refactor causes use-after-free.

**Fix**: Use `std::vector<std::string>` copies, or build a stable local buffer:
```cpp
// Replace the ptrs pattern:
std::vector<std::string> strs(vals.begin(), vals.end());  // ensure stable storage
std::vector<const char*> ptrs;
ptrs.reserve(strs.size());
for (const auto& s : strs) ptrs.push_back(s.c_str());
```
Or better: check if HDF5 C++ VL write API accepts `std::string*` range directly.

**Acceptance**: `ptrs` storage lifetime independent of `vals` lifetime.

---

## TASK 6 — Fix TOCTOU in `checkMergeRotation()` [MAJOR]

**Files**: `HDF5Writer.cpp` lines 1201–1218

**Problem**: Releases `mergeFileMutex_` after `needRotate=true`, then calls `rotateMergeFile()` without holding lock. Another `appendFrameMerge()` can write between check and rotate. Two concurrent calls could double-rotate.

**Fix**: Add `std::atomic<bool> mergeRotating_{false}` member. In `checkMergeRotation()`:
```cpp
bool expected = false;
if (needRotate && mergeRotating_.compare_exchange_strong(expected, true)) {
    rotateMergeFile();
    mergeRotating_.store(false);
}
```

**Acceptance**: Concurrent stress test never rotates twice per threshold crossing.

---

## TASK 7 — Fix silent data drop on tabular round-change [MAJOR]

**Files**: `HDF5Writer.cpp` lines 836–849

**Problem**: In `accumulateTabularFrame()`, if `frameFirstTs != buf.roundFirstTs` and `buf.rowCount > 0`, buffer is cleared without flushing. Data silently lost if sources publish at different rates.

**Fix**: Before clearing buffer, flush it:
```cpp
if (buf.rowCount > 0 && frameFirstTs != -1 && frameFirstTs != buf.roundFirstTs)
{
    // Flush stale partial round before starting new one.
    if (config_.mergeRootSources)
        flushTabularBufferMerge(sourceName, buf);
    else {
        auto ev = pool_->acquire(sourceName);
        std::lock_guard<std::mutex> fileLk(ev->fileMutex);
        flushTabularBuffer(sourceName, buf, ev->file);
    }
    // buf is now cleared by flush; no need to manually clear.
}
```
Verify flush already resets `rowCount` — it does (line 1063). Remove manual clear block.

**Acceptance**: Partial round data written before new round starts.

---

## TASK 8 — Reset `buf.roundFirstTs` after flush [MINOR]

**Files**: `HDF5Writer.cpp` line 1063

**Problem**: `flushTabularBuffer()` sets `rowCount=0` but leaves `roundFirstTs` at stale value. Misleading on future reads.

**Fix**: Add `buf.roundFirstTs = -1;` alongside `buf.rowCount = 0;` in `flushTabularBuffer()`.

---

## TASK 9 — Remove `ensure1D`/`ensure2D` no-op lambdas in `appendFrame()` [MINOR]

**Files**: `HDF5Writer.cpp` lines 612–619

**Problem**: Lambdas are pure pass-throughs adding a call level for zero benefit.

**Fix**: Call `ensureDataset(file, ...)` and `ensureDataset2D(file, ...)` directly throughout `appendFrame()`. Already done this way in `appendFrameMerge()`.

---

## TASK 10 — `ensureMergeGroup()` return `void` [MINOR]

**Files**: `HDF5Writer.cpp` / `HDF5Writer.h` lines 1185–1199

**Problem**: Returns `H5::Group` by value; all callers discard it → `H5Gclose` immediately. Misleading return type.

**Fix**: Change signature to `void ensureMergeGroup(const std::string& sourceName)`. Remove `return` statement.

---

## TASK 11 — Bound `warnedUnknown` set [MINOR]

**Files**: `HDF5Writer.cpp` lines 872, 975; `HDF5Writer.h` (TabularBuffer struct)

**Problem**: `warnedUnknown` set grows indefinitely. Long-running process + schema drift = unbounded memory.

**Fix**: Cap at e.g. 128 entries. If full, log once ("warning set full") and stop inserting. Or clear periodically (e.g. every N flushes).

---

## TASK 12 — Hoist `pool_->acquire()` outside per-frame inner loop [MINOR PERF]

**Files**: `HDF5Writer.cpp` line 351

**Problem**: `pool_->acquire()` called per frame for same source → repeated pool mutex lock/unlock.

**Fix**: Group frames by `root_source` before the inner loop and acquire once per source per drain cycle. Or at minimum acquire once outside the `for (const auto& frame : entry.batch.frames)` loop since all frames in a batch share the same `root_source`.

---

## TASK 13 — Add proper mutex docs for `lastTsBatchSeq_` and `tabularBuffers_` [MINOR]

**Files**: `HDF5Writer.h`, `HDF5Writer.cpp` header comment (lines 43–55)

**Problem**: Locking discipline comment omits `lastTsBatchSeq_` and `tabularBuffers_`. Future developers may incorrectly add concurrent access.

**Fix**: Add explicit comment: both maps are `writerThread_`-exclusive; no mutex needed today but must be reviewed before any multi-threaded writer refactor.

---

## Implementation Order

| # | Task | Type | Effort |
|---|------|------|--------|
| 1 | Deduplicate visit lambda | Refactor | L |
| 2 | Fix `mergeBytesWritten_` tabular | Bug | S |
| 3 | Fix queue depth metric | Bug | XS |
| 4 | Fix `append1D/2D` getSpace | Clarity+perf | S |
| 5 | Fix VL string pointer | Safety | S |
| 6 | Fix TOCTOU rotation | Bug | S |
| 7 | Fix silent tabular data drop | Bug | M |
| 8 | Reset `roundFirstTs` after flush | Correctness | XS |
| 9 | Remove no-op lambdas | Cleanup | XS |
| 10 | `ensureMergeGroup` void return | Cleanup | XS |
| 11 | Bound `warnedUnknown` | Safety | XS |
| 12 | Hoist `pool_->acquire()` | Perf | S |
| 13 | Document thread ownership | Docs | XS |

Start with Task 1 — it restructures the file and makes subsequent fixes land in one place instead of two.
