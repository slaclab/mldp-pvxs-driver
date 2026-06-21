# Integration Test: HDF5BsasGen1Reader Provenance → Controller → MLDPWriter

## Goal

Verify provenance metadata configured in HDF5BsasGen1Reader YAML flows through
the full pipeline (reader → controller → MLDP writer → gRPC) and arrives in
DataFrame column attributes.

## Data Flow Under Test

```
HDF5BsasGen1Reader (provenance: in YAML)
  → EventBatch.metadata["provenance.facility"] = "LCLS"
  → Controller.push()
  → MLDPWriter.push()
  → QueueItem.metadata (shared_ptr)
  → toDataFrame() → apply_metadata lambda
  → DataFrame column.metadata.attributes["provenance.facility"] = "LCLS"
  → gRPC IngestDataRequest
```

## Implementation

### File

`test/controller/mldppvxs_controller_hdf5_provenance_test.cpp`

### Pattern to Follow

Based on `test/controller/mldppvxs_controller_mldp_writer_integration_test.cpp` and
`test/common/MldpQueryTestUtils.h`:

1. **Real gRPC services** — dev container runs `dp-ingestion:50051` and `dp-query:50052`
2. **YAML config** — wire HDF5 reader + MLDP writer pointing to real services
3. **Controller lifecycle** — `MLDPPVXSController::create(cfg)` → `start()` → wait → `stop()`
4. **Query verification** — use `queryAndCollectColumns()` from `MldpQueryTestUtils.h` to read back ingested data and check provenance attributes

### Test: `ProvenanceFlowsFromHDF5ReaderThroughControllerToMLDP`

```cpp
TEST(ControllerHDF5ProvenanceTest, ProvenanceFlowsToMLDPWriter)
{
    // 1. Generate mock HDF5 file
    auto tempDir = fs::temp_directory_path() / "hdf5_prov_test";
    fs::create_directories(tempDir);
    std::string mockFile = (tempDir / "provenance_test.h5").string();
    BsasGen1HDF5Mock::Params params{.numFloatCols=3, .numIntCols=2, .numRows=5, .baseEpoch=1700000000};
    BsasGen1HDF5Mock::generate(mockFile, params);

    // 2. Build YAML config: HDF5 reader with provenance → real MLDP services
    std::ostringstream yaml;
    yaml << "writer:\n"
         << "  mldp:\n"
         << "    - name: mldp_prov\n"
         << "      mldp-pool:\n"
         << "        provider-name: prov-test-provider\n"
         << "        ingestion-url: dp-ingestion:50051\n"
         << "        query-url: dp-query:50052\n"
         << "        min-conn: 1\n"
         << "        max-conn: 1\n"
         << "reader:\n"
         << "  hdf5-bsas-gen1:\n"
         << "    - name: bsas_prov_test\n"
         << "      file-path: " << mockFile << "\n"
         << "      chunk-size: 1000\n"
         << "      provenance:\n"
         << "        facility: LCLS\n"
         << "        instrument: CXI\n"
         << "        subsystem: BSAS\n";

    auto config = makeConfigFromYaml(yaml.str());
    ASSERT_TRUE(config.valid());

    // 3. Create and run controller (ingests to real dp-ingestion:50051)
    auto controller = MLDPPVXSController::create(config);
    ASSERT_TRUE(controller);
    controller->start();

    // 4. Query back from real dp-query:50052 and verify provenance
    std::vector<std::string> pvNames = {"bsas_prov_test/float_0", "bsas_prov_test/float_1"};
    auto columns = queryAndCollectColumns(pvNames, std::chrono::seconds(10));
    ASSERT_FALSE(columns.empty());

    controller->stop();

    // 5. Verify provenance attributes on returned columns
    for (const auto& col : columns)
    {
        const auto& attrs = col.metadata().attributes();
        EXPECT_TRUE(hasAttr(attrs, "provenance.facility", "LCLS"));
        EXPECT_TRUE(hasAttr(attrs, "provenance.instrument", "CXI"));
        EXPECT_TRUE(hasAttr(attrs, "provenance.subsystem", "BSAS"));
        EXPECT_TRUE(hasAttr(attrs, "source", "bsas_prov_test"));
    }

    // Cleanup
    fs::remove_all(tempDir);
}
```

### Test: `MissingProvenanceNoExtraAttributes`

Same setup but YAML has NO `provenance:` section. Verify no `"provenance.*"` keys
in DataFrame column attributes.

### Dependencies (includes)

```cpp
#include <gtest/gtest.h>
#include <controller/MLDPPVXSController.h>
#include <grpcpp/grpcpp.h>

#include "config/test_config_helpers.h"
#include "mock/BsasGen1HDF5Mock.h"
#include "common/MldpQueryTestUtils.h"
```

### CMake Target

Add to `test/CMakeLists.txt`:

```cmake
if(MLDP_PVXS_ENABLE_HDF5)
    add_executable(mldp_controller_hdf5_provenance_test
        controller/mldppvxs_controller_hdf5_provenance_test.cpp
        mock/BsasGen1HDF5Mock.cpp)
    target_link_libraries(mldp_controller_hdf5_provenance_test
        mldp_pvxs_driver gtest gtest_main ${GRPC_LIBRARIES})
    gtest_discover_tests(mldp_controller_hdf5_provenance_test)
endif()
```

### Key Files to Reference

| File | What to Reuse |
|------|---------------|
| `test/common/MldpQueryTestUtils.h:174` | `queryAndCollectColumns()` — queries real `dp-query:50052` |
| `test/controller/mldppvxs_controller_mldp_writer_integration_test.cpp:33-48` | YAML config with real service URLs pattern |
| `test/controller/mldppvxs_controller_hdf5_integration_test.cpp:92-98` | Controller create/start/stop pattern |
| `test/mock/BsasGen1HDF5Mock.h` | HDF5 mock file generator |
| `src/writer/mldp/MLDPWriter.cpp:621-632` | `apply_metadata` lambda (where metadata → attributes) |
| `src/controller/MLDPPVXSController.cpp:361-462` | Controller push() fan-out preserves metadata |

### Verification

```bash
# In devcontainer:
cmake --build build --target mldp_controller_hdf5_provenance_test
./build/bin/mldp_controller_hdf5_provenance_test
```

Both tests pass → provenance flows end-to-end.
