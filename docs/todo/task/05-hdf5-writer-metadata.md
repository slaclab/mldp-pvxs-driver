# TODO-05: Write `batch.metadata` as HDF5 group attributes in HDF5WriterBase

## Goal
When `HDF5WriterBase` creates a new source group, write each key-value pair from
`batch.metadata` as a string attribute on that group.

## Depends On
- TODO-01 (`batch.metadata` exists)

## Before doing this
1. Find the exact HighFive attribute write call pattern already used in the codebase:
   ```bash
   grep -n "createAttribute\|getAttribute\|Attribute" src/writer/hdf5/HDF5WriterBase.cpp | head -20
   grep -n "createAttribute\|getAttribute\|Attribute" include/writer/hdf5/HDF5WriterBase.h | head -10
   ```
   Typical HighFive pattern for a string attribute:
   ```cpp
   group.createAttribute<std::string>("key", HighFive::DataSpace::From(value)).write(value);
   // or the simpler overload if available:
   group.createAttribute("key", value);
   ```

2. Identify where source groups are created in `HDF5WriterBase.cpp`:
   ```bash
   grep -n "require_group\|createGroup\|openGroup\|seen_groups\|source_group" src/writer/hdf5/HDF5WriterBase.cpp | head -20
   ```

## Files to Change

### `src/writer/hdf5/HDF5WriterBase.cpp`
- Locate the source group creation point (first time a root_source is seen)
- A `std::set<std::string> seen_groups_` (or equivalent) should track which groups already
  had attributes written. If not present, add it as a private member in the header.
- After group creation, if it is first-time-seen:
  ```cpp
  if (seen_groups_.insert(batch.root_source).second) {
      auto group = file_.require_group(batch.root_source);   // adjust to actual variable name
      for (auto& [k, v] : batch.metadata)
          group.createAttribute(k, v);   // verify exact HighFive overload
  }
  ```

### `include/writer/hdf5/HDF5WriterBase.h` (if `seen_groups_` not yet present)
- Add: `std::set<std::string> seen_groups_;`
- Add `#include <set>` if needed

## Verification
```bash
cmake --build build 2>&1 | grep -E "error:" | head -20
# Run HDF5 writer tests
ctest --test-dir build -R "hdf5_writer" -V 2>&1 | tail -30
# Manual verification: h5dump the output file and check group attributes
# h5dump -A output.h5 | grep -A5 "GROUP"
```

## Commit
```
feat(metadata): write batch metadata as HDF5 source group attributes

HDF5WriterBase now writes each EventBatch::metadata key-value pair as
a string attribute on the source group the first time that group is
created, making reader and PV static metadata queryable via h5dump.
```
