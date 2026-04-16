# TODO: Controller + EPICS → HDF5 Integration Tests

## Goal

Add controller-level integration tests that wire a **mock EPICS PVXS soft-IOC**
(already used in existing reader tests) to the **HDF5 writer**.  Each test starts a
real `MLDPPVXSController`, subscribes to one PV type served by the mock IOC, lets
data flow, then opens the resulting HDF5 file with the C++ HDF5 API and verifies
correctness of the written datasets.

---

## Context

| Component | Where |
|---|---|
| Mock PVXS soft-IOC | `test/mock/sioc.cpp` — already serves all scalar + array PV types |
| PV type catalog | `test/mock/sioc.cpp` lines ~81–196 (Bool → StringA, plus NTTable / BSAS) |
| Existing controller integration test | `test/controller/mldppvxs_controller_test.cpp` |
| HDF5 writer integration test | `test/writer/hdf5/hdf5_writer_test.cpp` |
| Controller config helper | `test/controller/mldppvxs_controller_test.cpp` — `startControllerWithReaderSection()` |

---

## Tasks

### 1. New test file

**File:** `test/controller/hdf5/mldppvxs_controller_hdf5_integration_test.cpp`

Guard the entire file with `#ifdef MLDP_PVXS_HDF5_ENABLED`.

### 2. Fixture

```
class ControllerHDF5Test : public ::testing::Test {
    // - spin up mock PVXS IOC (reuse existing pattern from epics_pvxs_reader_test.cpp)
    // - create temp HDF5 output dir in SetUp()
    // - build YAML config: epics-pvxs reader + hdf5 writer pointing at temp dir
    // - start controller in SetUp(), stop + remove temp dir in TearDown()
};
```

Helper `makeConfig(pvName, tempDir)` builds the minimal YAML string:
```yaml
name: ctrl-hdf5-test
reader:
  epics-pvxs:
    - name: <pvName>
      channel: <pvName>
writer:
  hdf5:
    - name: hdf5-out
      base-path: <tempDir>
      max-file-age-s: 3600
      max-file-size-mb: 512
```

### 3. One test per scalar PV type

For each type below, verify:
1. After N monitor updates (≥ 3), at least one `.h5` file exists in temp dir.
2. Open file with `H5::H5File`; dataset for the PV exists.
3. Dataset has ≥ 1 row (non-empty).
4. Spot-check: value column contains the expected scalar value published by mock IOC.

| Test name | PV | TypeCode |
|---|---|---|
| `ScalarBool` | `test:bool` | `Bool` |
| `ScalarInt8` | `test:int8` | `Int8` |
| `ScalarInt16` | `test:int16` | `Int16` |
| `ScalarInt32` | `test:int32` | `Int32` |
| `ScalarInt64` | `test:int64` | `Int64` |
| `ScalarUInt8` | `test:uint8` | `UInt8` |
| `ScalarUInt16` | `test:uint16` | `UInt16` |
| `ScalarUInt32` | `test:uint32` | `UInt32` |
| `ScalarUInt64` | `test:uint64` | `UInt64` |
| `ScalarFloat32` | `test:float32` | `Float32` |
| `ScalarFloat64` | `test:float64` | `Float64` |
| `ScalarString` | `test:string` | `String` |

### 4. One test per array PV type

Same verification pattern but:
- Dataset has array column with correct element count.
- Spot-check first element value.

| Test name | PV | TypeCode |
|---|---|---|
| `ArrayBool` | `test:bool_array` | `BoolA` |
| `ArrayInt8` | `test:int8_array` | `Int8A` |
| `ArrayInt16` | `test:int16_array` | `Int16A` |
| `ArrayInt32` | `test:int32_array` | `Int32A` |
| `ArrayInt64` | `test:int64_array` | `Int64A` |
| `ArrayUInt8` | `test:uint8_array` | `UInt8A` |
| `ArrayUInt16` | `test:uint16_array` | `UInt16A` |
| `ArrayUInt32` | `test:uint32_array` | `UInt32A` |
| `ArrayUInt64` | `test:uint64_array` | `UInt64A` |
| `ArrayFloat32` | `test:float32_array` | `Float32A` |
| `ArrayFloat64` | `test:float64_array` | `Float64A` |
| `ArrayString` | `test:string_array` | `StringA` |

### 5. NTTable test

| Test name | PV | Notes |
|---|---|---|
| `NTTablePressure` | `test:table` (NTTable with `deviceIDs`/`pressure` cols) | Verify both column datasets written |

### 6. CMakeLists.txt

Add new test target inside both `if(MLDP_PVXS_DRIVER_TESTS)` and
`if(MLDP_PVXS_ENABLE_HDF5)`:

```cmake
if(MLDP_PVXS_ENABLE_HDF5)
    add_executable(mldp_controller_hdf5_integration_test
        "${CMAKE_CURRENT_SOURCE_DIR}/test/controller/hdf5/mldppvxs_controller_hdf5_integration_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/mock/sioc.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/ryml_single_header_translation_unit.cpp"
    )
    target_include_directories(mldp_controller_hdf5_integration_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/test"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    # --whole-archive preserves REGISTER_WRITER/REGISTER_READER static init
    if(APPLE)
        target_link_libraries(mldp_controller_hdf5_integration_test PRIVATE gtest_main
            -Wl,-force_load $<TARGET_FILE:lib${PROJECT_NAME}>)
    else()
        target_link_libraries(mldp_controller_hdf5_integration_test PRIVATE gtest_main
            -Wl,--whole-archive lib${PROJECT_NAME} -Wl,--no-whole-archive)
    endif()
    gtest_discover_tests(mldp_controller_hdf5_integration_test
        PROPERTIES LABELS "hdf5;controller;integration")
endif()
```

---

## Key files to read before implementing

| File | Why |
|---|---|
| `test/mock/sioc.cpp` | PV names, TypeCodes, published values |
| `test/mock/sioc.h` | SIOC fixture API |
| `test/controller/mldppvxs_controller_test.cpp` | How to start/stop controller with YAML config |
| `test/writer/hdf5/hdf5_writer_test.cpp` | How to open/inspect HDF5 output files |
| `include/writer/hdf5/HDF5Writer.h` | Dataset layout / group structure |
| `src/writer/hdf5/HDF5Writer.cpp` | Dataset naming convention (PV name → HDF5 path) |

---

## Constraints

- **Guard**: entire file inside `#ifdef MLDP_PVXS_HDF5_ENABLED`.
- **GCC, Rocky Linux 9** — no Clang-specific extensions.
- **No live EPICS**: use mock IOC only. Tests must pass offline.
- **Timing**: use `std::this_thread::sleep_for` + retry loop (max ~2 s) rather than fixed
  sleeps to wait for data to arrive; prefer `ASSERT_EVENTUALLY` pattern if already used
  in existing tests.
- **Do NOT compare HDF5 file paths with timestamp suffix** — `nowUtcFileSuffix()` has
  1-second resolution; check dataset contents instead.
- Match license header and include style of existing test files.
