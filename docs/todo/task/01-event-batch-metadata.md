# TODO-01: Replace `tags` with `metadata` in EventBatchStruct

## Goal
Replace `vector<string> tags` with `unordered_map<string,string> metadata` in `EventBatchStruct`.
This is a breaking rename — all call sites must update in the same commit so the build stays green.

## Files to Change

### Core struct
- `include/util/bus/IDataBus.h`
  - `EventBatchStruct::tags` → `metadata`; type `vector<string>` → `unordered_map<string,string>`
  - Remove or update the doc comment on the field (was "Optional metadata attached to the batch")
  - Add `#include <unordered_map>` if not present

### MLDPWriter (QueueItem carries the tags today)
- `include/writer/mldp/MLDPWriter.h`
  - `QueueItem::tags` field: `shared_ptr<const vector<string>>` → `shared_ptr<const unordered_map<string,string>>`
- `src/writer/mldp/MLDPWriter.cpp` line ~178
  - `auto tags = make_shared<const vector<string>>(batch.tags)` → `make_shared<const unordered_map<string,string>>(batch.metadata)`

### Reader impls (all use `.tags.push_back(...)` — replace with `.metadata["source"] = ...` or just clear)
For now these just need to compile. They will be wired with real metadata in TODO-03.
Replace each `batch.tags.push_back(X)` with `batch.metadata["source"] = X` as a
minimal placeholder so the build stays green.

- `src/reader/impl/epics_archiver/EpicsArchiverReader.cpp` line ~330
- `src/reader/impl/epics/pvxs/EpicsPVXSReader.cpp` lines ~175, ~200, ~208, ~255
- `src/reader/impl/epics/base/EpicsBaseReader.cpp` lines ~177, ~200, ~207

### Tests (replace `.tags` assignments with `.metadata`)
- `test/controller/mldppvxs_controller_mldp_writer_integration_test.cpp` lines ~307, ~460, ~477
  - `batch.tags = {}` → `batch.metadata = {}`
  - `batch.tags = {"test"}` → `batch.metadata = {{"source", "test"}}`
- `test/writer/hdf5/hdf5_writer_test.cpp` lines ~1071, ~1105, ~1145, ~1783, ~1829, ~1848, ~1904, ~2023, ~2045
  - `batch.tags = {pvName}` → `batch.metadata = {{"source", pvName}}`
  - `batch.tags.push_back(pvName)` → `batch.metadata["source"] = pvName`

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:|warning:" | head -30
# Must compile clean. No `.tags` references remain.
grep -rn "\.tags" src/ include/ test/ --include="*.cpp" --include="*.h"
```

## Commit
```
feat(metadata): replace EventBatchStruct::tags with metadata map

Replace vector<string> tags with unordered_map<string,string> metadata
in EventBatchStruct and all dependent code. Reader impls use a temporary
"source" key placeholder; full metadata wiring follows in subsequent todos.
```
