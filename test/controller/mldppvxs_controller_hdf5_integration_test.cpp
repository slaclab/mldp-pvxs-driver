//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include <controller/MLDPPVXSController.h>
#include <H5Cpp.h>

#include "../mock/sioc.h"
#include "../config/test_config_helpers.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using mldp_pvxs_driver::config::makeConfigFromYaml;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// YAML config builder
// ---------------------------------------------------------------------------

static std::string buildYaml(const std::string& pvName, const std::string& basePath)
{
    return std::string(
        "writer:\n"
        "  hdf5:\n"
        "    - name: hdf5-test\n"
        "      base-path: \"") + basePath + "\"\n"
        "      flush-interval-ms: 100\n"
        "reader:\n"
        "  - epics-pvxs:\n"
        "      - name: test-reader\n"
        "        pvs:\n"
        "          - name: " + pvName + "\n";
}

static std::string buildBsasTableYaml(const std::string& pvName, const std::string& basePath)
{
    return std::string(
        "writer:\n"
        "  hdf5:\n"
        "    - name: hdf5-test\n"
        "      base-path: \"") + basePath + "\"\n"
        "      flush-interval-ms: 100\n"
        "reader:\n"
        "  - epics-pvxs:\n"
        "      - name: test-reader\n"
        "        pvs:\n"
        "          - name: " + pvName + "\n"
        "            option:\n"
        "              type: slac-bsas-table\n"
        "              tsSeconds: secondsPastEpoch\n"
        "              tsNanos: nanoseconds\n";
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ControllerHDF5Test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = fs::temp_directory_path() / "ctrl_hdf5_test" / info->test_suite_name() / info->name();
        fs::create_directories(tempDir_);
        pvServer_ = std::make_unique<PVServer>();
    }

    void TearDown() override
    {
        if (controller_)
        {
            controller_->stop();
            controller_.reset();
        }
        pvServer_.reset();
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    void startController(const std::string& pvName)
    {
        auto cfg = makeConfigFromYaml(buildYaml(pvName, tempDir_.string()));
        controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(cfg);
        ASSERT_TRUE(controller_) << "Failed to create controller for PV: " << pvName;
        controller_->start();
    }

    fs::path waitForH5File(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            for (const auto& e : fs::recursive_directory_iterator(tempDir_))
            {
                if (e.is_regular_file() && e.path().extension() == ".hdf5")
                    return e.path();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return {};
    }

    std::unique_ptr<PVServer>                                          pvServer_;
    std::shared_ptr<mldp_pvxs_driver::controller::MLDPPVXSController> controller_;
    fs::path                                                           tempDir_;
};

// ---------------------------------------------------------------------------
// Shared verification helper
// ---------------------------------------------------------------------------

static void checkH5HasTimestamps(const fs::path& h5path)
{
    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("timestamps")) << "No 'timestamps' dataset in " << h5path;
    hsize_t   dims[1]{0};
    H5::DataSet   ds = file.openDataSet("timestamps");
    H5::DataSpace sp = ds.getSpace();
    sp.getSimpleExtentDims(dims);
    EXPECT_GT(dims[0], 0u) << "'timestamps' dataset is empty in " << h5path;
}

// ---------------------------------------------------------------------------
// Parameterized fixture — shared between scalar and array PV tests
// ---------------------------------------------------------------------------

class PvHDF5Test : public ControllerHDF5Test,
                   public ::testing::WithParamInterface<std::string>
{
};

TEST_P(PvHDF5Test, WritesNonEmptyTimestamps)
{
    startController(GetParam());
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No .h5 file written for PV: " << GetParam();
    controller_->stop();
    controller_.reset();
    checkH5HasTimestamps(h5path);
}

// Scalar PV types
INSTANTIATE_TEST_SUITE_P(ScalarTypes,
                         PvHDF5Test,
                         ::testing::Values("test:bool",
                                           "test:int8",
                                           "test:int16",
                                           "test:int32",
                                           "test:int64",
                                           "test:uint8",
                                           "test:uint16",
                                           "test:uint32",
                                           "test:uint64",
                                           "test:float32",
                                           "test:float64",
                                           "test:string"));

// Array PV types
INSTANTIATE_TEST_SUITE_P(ArrayTypes,
                         PvHDF5Test,
                         ::testing::Values("test:bool_array",
                                           "test:int8_array",
                                           "test:int16_array",
                                           "test:int32_array",
                                           "test:int64_array",
                                           "test:uint8_array",
                                           "test:uint16_array",
                                           "test:uint32_array",
                                           "test:uint64_array",
                                           "test:float32_array",
                                           "test:float64_array",
                                           "test:string_array"));

// ---------------------------------------------------------------------------
// Data-value readback tests
// ---------------------------------------------------------------------------

TEST_F(ControllerHDF5Test, VoltagePVDataStoredInHDF5)
{
    // test:voltage publishes Float64 NTScalar: 1.5 + sin(time), range (0.5, 2.5)
    startController("test:voltage");
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No .h5 file written for test:voltage";
    controller_->stop();
    controller_.reset();

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);

    // timestamps dataset must exist and be non-empty
    ASSERT_TRUE(file.nameExists("timestamps")) << "'timestamps' dataset missing";
    {
        H5::DataSet   tsDs = file.openDataSet("timestamps");
        H5::DataSpace tsSp = tsDs.getSpace();
        hsize_t       tsDims[1]{0};
        tsSp.getSimpleExtentDims(tsDims);
        EXPECT_GT(tsDims[0], 0u) << "'timestamps' dataset is empty";
    }

    // value column must exist with at least one sample in the expected range
    ASSERT_TRUE(file.nameExists("test:voltage")) << "'test:voltage' dataset missing";
    H5::DataSet   ds = file.openDataSet("test:voltage");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[1]{0};
    sp.getSimpleExtentDims(dims);
    ASSERT_GT(dims[0], 0u) << "'test:voltage' dataset is empty";

    std::vector<double> values(dims[0]);
    ds.read(values.data(), H5::PredType::NATIVE_DOUBLE);
    for (const auto v : values)
    {
        EXPECT_GT(v, 0.4) << "voltage sample out of expected range: " << v;
        EXPECT_LT(v, 2.6) << "voltage sample out of expected range: " << v;
    }
}

TEST_F(ControllerHDF5Test, CounterPVDataStoredInHDF5)
{
    // test:counter publishes Int32 NTScalar: monotonically increasing counter (>0)
    startController("test:counter");
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No .h5 file written for test:counter";
    controller_->stop();
    controller_.reset();

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);

    ASSERT_TRUE(file.nameExists("timestamps")) << "'timestamps' dataset missing";
    {
        H5::DataSet   tsDs = file.openDataSet("timestamps");
        H5::DataSpace tsSp = tsDs.getSpace();
        hsize_t       tsDims[1]{0};
        tsSp.getSimpleExtentDims(tsDims);
        EXPECT_GT(tsDims[0], 0u) << "'timestamps' dataset is empty";
    }

    ASSERT_TRUE(file.nameExists("test:counter")) << "'test:counter' dataset missing";
    H5::DataSet   ds = file.openDataSet("test:counter");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[1]{0};
    sp.getSimpleExtentDims(dims);
    ASSERT_GT(dims[0], 0u) << "'test:counter' dataset is empty";

    std::vector<int32_t> values(dims[0]);
    ds.read(values.data(), H5::PredType::NATIVE_INT32);
    for (const auto v : values)
    {
        EXPECT_GT(v, 0) << "counter sample should be positive: " << v;
    }
}

// ---------------------------------------------------------------------------
// BSAS NTTable structural-correctness test
//
// Verifies that after stopping the controller the HDF5 file written from a
// live BSAS NTTable PV satisfies ALL of the following invariants:
//
//  1. File exists and is a valid HDF5 file.
//  2. 'timestamps' dataset is present and non-empty.
//  3. 'PV_A' (Float64), 'PV_B' (Int32), 'PV_C' (Float32) are present.
//  4. All four datasets have the SAME row count — split-column dedup worked.
//  5. Row count is a multiple of 3 (mock emits exactly 3 rows per update).
//  6. 'timestamps' values are plausible nanosecond-epoch values (> 0).
//  7. 'PV_A' (double): mock value = 1.0 + sin(time), range (0.0, 2.1).
//  8. 'PV_B' (int32):  mock value = counter + row_index, strictly > 0.
//  9. 'PV_C' (float):  mock value = 1.25 + cos(time), range (0.2, 2.3).
// ---------------------------------------------------------------------------

static std::string buildBsasTableDetailYaml(const std::string& pvName,
                                            const std::string& basePath)
{
    return std::string(
               "writer:\n"
               "  hdf5:\n"
               "    - name: hdf5-bsas-detail\n"
               "      base-path: \"") +
           basePath +
           "\"\n"
           "      flush-interval-ms: 50\n"
           "reader:\n"
           "  - epics-pvxs:\n"
           "      - name: bsas-detail-reader\n"
           "        pvs:\n"
           "          - name: " +
           pvName +
           "\n"
           "            option:\n"
           "              type: slac-bsas-table\n"
           "              tsSeconds: secondsPastEpoch\n"
           "              tsNanos: nanoseconds\n";
}

TEST_F(ControllerHDF5Test, BsasNTTableStructuralCorrectness)
{
    // Structural invariant under test
    // --------------------------------
    // Each BSAS NTTable event is converted by BSASEpicsMLDPConversion into a
    // single EventBatch whose `frames` vector holds one DataFrame per column:
    //
    //   EventBatch {
    //     root_source = "test:bsas_table",
    //     frames = [
    //       DataFrame { timestamps=[t0,t1,t2], PV_A=[...] },
    //       DataFrame { timestamps=[t0,t1,t2], PV_B=[...] },   ← same ts
    //       DataFrame { timestamps=[t0,t1,t2], PV_C=[...] },   ← same ts
    //     ]
    //   }
    //
    // All three frames carry identical timestamps.  The HDF5Writer must write
    // timestamps only once (via batchSeq dedup) so that:
    //
    //   timestamps.size() == PV_A.size() == PV_B.size() == PV_C.size()
    //
    // Without dedup:  timestamps.size() == 3 × column.size()  ← corrupt file.
    //
    // The mock publishes 3 rows per update; row counts must be multiples of 3.

    // ---- 1. Start controller and wait for enough data ----------------------
    auto cfg = makeConfigFromYaml(
        buildBsasTableDetailYaml("test:bsas_table", tempDir_.string()));
    controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(cfg);
    ASSERT_TRUE(controller_) << "Failed to create controller for test:bsas_table";
    controller_->start();

    // Wait for file creation then let a few more update cycles land so we
    // accumulate at least 2 update windows (≥6 rows) for a meaningful test.
    const auto h5path = waitForH5File(std::chrono::milliseconds(5000));
    ASSERT_FALSE(h5path.empty()) << "No .hdf5 file written within 5 s";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---- 2. Stop controller BEFORE opening/reading the file ----------------
    controller_->stop();
    controller_.reset();

    // ---- 3. Open file -------------------------------------------------------
    H5::H5File file;
    ASSERT_NO_THROW(file = H5::H5File(h5path.string(), H5F_ACC_RDONLY))
        << "HDF5 file cannot be opened: " << h5path;

    // ---- 4. All expected datasets exist ------------------------------------
    ASSERT_TRUE(file.nameExists("timestamps")) << "'timestamps' missing in " << h5path;
    ASSERT_TRUE(file.nameExists("PV_A"))       << "'PV_A' (Float64) missing";
    ASSERT_TRUE(file.nameExists("PV_B"))       << "'PV_B' (Int32)   missing";
    ASSERT_TRUE(file.nameExists("PV_C"))       << "'PV_C' (Float32) missing";

    // ---- 5. Read dimensions -------------------------------------------------
    const auto getDim1D = [&](const std::string& name) -> hsize_t {
        hsize_t dims[1]{0};
        file.openDataSet(name).getSpace().getSimpleExtentDims(dims);
        return dims[0];
    };

    const hsize_t nTs = getDim1D("timestamps");
    const hsize_t nA  = getDim1D("PV_A");
    const hsize_t nB  = getDim1D("PV_B");
    const hsize_t nC  = getDim1D("PV_C");

    // ---- 6. Non-empty -------------------------------------------------------
    ASSERT_GT(nTs, 0u) << "'timestamps' dataset is empty";

    // ---- 7. PRIMARY: all datasets have the same row count -------------------
    // This is the dedup correctness assertion.  Each BSAS event produces one
    // EventBatch with 3 DataFrames (PV_A, PV_B, PV_C), each carrying the same
    // timestamp array.  batchSeq dedup must write timestamps exactly once per
    // batch so all datasets grow in lockstep.
    //
    // Failure means: timestamps written N-times (once per column DataFrame)
    // instead of once per batch → timestamps.size() = N × column.size().
    EXPECT_EQ(nA, nTs)
        << "PV_A rows=" << nA << " ≠ timestamps rows=" << nTs
        << " — batchSeq dedup broken: timestamps written once per DataFrame"
           " instead of once per EventBatch";
    EXPECT_EQ(nB, nTs)
        << "PV_B rows=" << nB << " ≠ timestamps rows=" << nTs
        << " — batchSeq dedup broken";
    EXPECT_EQ(nC, nTs)
        << "PV_C rows=" << nC << " ≠ timestamps rows=" << nTs
        << " — batchSeq dedup broken";

    // ---- 8. Row count is a multiple of 3 (mock emits 3 rows per event) -----
    EXPECT_EQ(nTs % 3, 0u)
        << "timestamps row count " << nTs << " is not a multiple of 3"
           " — partial write or off-by-one in split logic";

    // ---- 9. Timestamps: positive and monotone within each event window ------
    // mock sets per-row nanos = base_nanos + row_index, so within each window
    // of 3 consecutive rows the timestamps must be strictly increasing.
    {
        std::vector<int64_t> tsVals(nTs);
        file.openDataSet("timestamps").read(tsVals.data(), H5::PredType::NATIVE_INT64);

        for (hsize_t i = 0; i < nTs; ++i)
        {
            EXPECT_GT(tsVals[i], 0LL)
                << "timestamps[" << i << "] = " << tsVals[i] << " is not positive";
        }

        for (hsize_t g = 0; g + 2 < nTs; g += 3)
        {
            EXPECT_LT(tsVals[g], tsVals[g + 1])
                << "timestamps not monotone within event window at row " << g;
            EXPECT_LT(tsVals[g + 1], tsVals[g + 2])
                << "timestamps not monotone within event window at row " << g + 1;
        }
    }

    // ---- 10. PV_A (double) sanity-check ------------------------------------
    // mock: row k sends (k+1) + sin(time), k∈{0,1,2} → outer bounds (0.0, 4.01)
    {
        std::vector<double> vals(nA);
        file.openDataSet("PV_A").read(vals.data(), H5::PredType::NATIVE_DOUBLE);
        for (hsize_t i = 0; i < nA; ++i)
        {
            EXPECT_GT(vals[i], 0.0)
                << "PV_A[" << i << "] = " << vals[i] << " out of range";
            EXPECT_LT(vals[i], 4.01)
                << "PV_A[" << i << "] = " << vals[i] << " out of range";
        }
    }

    // ---- 11. PV_B (int32) sanity-check -------------------------------------
    // mock: counter + row_index, counter > 0 throughout
    {
        std::vector<int32_t> vals(nB);
        file.openDataSet("PV_B").read(vals.data(), H5::PredType::NATIVE_INT32);
        for (hsize_t i = 0; i < nB; ++i)
        {
            EXPECT_GT(vals[i], 0)
                << "PV_B[" << i << "] = " << vals[i] << " should be positive";
        }
    }

    // ---- 12. PV_C (float) sanity-check -------------------------------------
    // mock: row k sends (k+1.25) + cos(time), k∈{0,1,2} → outer bounds (0.24, 4.27)
    {
        std::vector<float> vals(nC);
        file.openDataSet("PV_C").read(vals.data(), H5::PredType::NATIVE_FLOAT);
        for (hsize_t i = 0; i < nC; ++i)
        {
            EXPECT_GT(vals[i], 0.24f)
                << "PV_C[" << i << "] = " << vals[i] << " out of range";
            EXPECT_LT(vals[i], 4.27f)
                << "PV_C[" << i << "] = " << vals[i] << " out of range";
        }
    }
}

