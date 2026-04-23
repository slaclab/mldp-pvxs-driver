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
        outputDir_ = fs::path(MLDP_TEST_DATA_DIR) / "hdf5" / info->test_suite_name() / info->name();
        fs::create_directories(outputDir_);
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
    }

    void startController(const std::string& pvName)
    {
        auto cfg = makeConfigFromYaml(buildYaml(pvName, outputDir_.string()));
        controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(cfg);
        ASSERT_TRUE(controller_) << "Failed to create controller for PV: " << pvName;
        controller_->start();
    }

    // Wait for a *final* (non-dotfile) HDF5 file to appear.  These are created
    // by HDF5FilePool only after the file is renamed on close, i.e. after
    // stop() is called.  Call waitForActivity() first to confirm data is
    // flowing, then stop() the controller, then call this.
    fs::path waitForH5File(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            for (const auto& e : fs::recursive_directory_iterator(outputDir_))
            {
                if (e.is_regular_file() && e.path().extension() == ".hdf5"
                    && e.path().filename().string().front() != '.')
                    return e.path();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return {};
    }

    // Wait for a dotfile (temp HDF5) to appear, confirming data is flowing.
    // Returns the final path the file will have after rename (strip leading dot).
    fs::path waitForActivity(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            for (const auto& e : fs::recursive_directory_iterator(outputDir_))
            {
                if (e.is_regular_file() && e.path().extension() == ".hdf5"
                    && e.path().filename().string().front() == '.')
                    return e.path();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return {};
    }

    std::unique_ptr<PVServer>                                          pvServer_;
    std::shared_ptr<mldp_pvxs_driver::controller::MLDPPVXSController> controller_;
    fs::path                                                           outputDir_;
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
    const auto dotfile = waitForActivity();
    ASSERT_FALSE(dotfile.empty()) << "No .hdf5 temp file written for PV: " << GetParam();
    controller_->stop();
    controller_.reset();
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No final .hdf5 file after stop() for PV: " << GetParam();
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
    ASSERT_FALSE(waitForActivity().empty()) << "No .hdf5 temp file written for test:voltage";
    controller_->stop();
    controller_.reset();
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No final .hdf5 file after stop() for test:voltage";

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
    ASSERT_FALSE(waitForActivity().empty()) << "No .hdf5 temp file written for test:counter";
    controller_->stop();
    controller_.reset();
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No final .hdf5 file after stop() for test:counter";

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
// Gen1 NTTable compound-dataset structural correctness test
//
// Verifies that when the CU-HXR Gen1 BSAS NTTable mock PV is recorded
// through the HDF5 writer the resulting file contains a SINGLE compound
// dataset named "CU-HXR" with:
//
//  1. H5T_COMPOUND type.
//  2. Fields for at least the first few sanitized signal names.
//  3. Fields "secondsPastEpoch" and "nanoseconds" embedded in the compound.
//  4. Row count > 0 and a multiple of 3 (kRows per update).
//  5. No separate /timestamps dataset (timestamps are embedded per-row).
//  6. Float64 signal values in the expected sinusoidal range (0.0, 2.1).
//
// Signal names come from data/signals.cu-hxr.prod.
// EPICS colons are sanitized to underscores: "ACCL:IN20:300:L0A_ACUHBR" →
// "ACCL_IN20_300_L0A_ACUHBR".
// ---------------------------------------------------------------------------

static std::string buildGen1TableYaml(const std::string& basePath)
{
    return std::string(
               "writer:\n"
               "  hdf5:\n"
               "    - name: hdf5-gen1\n"
               "      base-path: \"") +
           basePath +
           "\"\n"
           "      flush-interval-ms: 50\n"
           "reader:\n"
           "  - epics-pvxs:\n"
           "      - name: gen1-reader\n"
           "        pvs:\n"
           "          - name: CU-HXR\n"
           "            option:\n"
           "              type: slac-bsas-table\n"
           "              tsSeconds: secondsPastEpoch\n"
           "              tsNanos: nanoseconds\n";
}

// Verifies that after stopping the controller the HDF5 file written from a
// live Gen1 NTTable PV uses the per-column group layout.
//
//  Layout: /<source>/secondsPastEpoch  int64 [N_rows]
//          /<source>/nanoseconds       int64 [N_rows]
//          /<source>/<colName>         typed [N_rows]  (one per column)
//
//  There are NO compound datasets, NO __columns, NO __timestamps datasets.
TEST_F(ControllerHDF5Test, Gen1NTTableWritesCompoundDataset)
{
    auto cfg = makeConfigFromYaml(buildGen1TableYaml(outputDir_.string()));
    controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(cfg);
    ASSERT_TRUE(controller_) << "Failed to create controller for CU-HXR";
    controller_->start();

    // Wait for activity (dotfile) then let a couple more update cycles accumulate.
    ASSERT_FALSE(waitForActivity(std::chrono::milliseconds(5000)).empty())
        << "No .hdf5 temp file written within 5 s";
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    controller_->stop();
    controller_.reset();
    const auto h5path = waitForH5File(std::chrono::milliseconds(3000));
    ASSERT_FALSE(h5path.empty()) << "No final .hdf5 file after stop() for CU-HXR";

    // ---- Open file -----------------------------------------------------------
    H5::H5File file;
    ASSERT_NO_THROW(file = H5::H5File(h5path.string(), H5F_ACC_RDONLY))
        << "Cannot open HDF5 file: " << h5path;

    // ---- 1. Group "CU-HXR" exists -------------------------------------------
    ASSERT_TRUE(file.nameExists("CU-HXR"))
        << "Group 'CU-HXR' not found in " << h5path;

    auto grp = file.openGroup("CU-HXR");

    // ---- 2. Timestamp datasets exist inside the group -----------------------
    ASSERT_TRUE(grp.nameExists("secondsPastEpoch"))
        << "Dataset 'CU-HXR/secondsPastEpoch' missing";
    ASSERT_TRUE(grp.nameExists("nanoseconds"))
        << "Dataset 'CU-HXR/nanoseconds' missing";

    // ---- 3. All sanitized signal column datasets present --------------------
    for (const auto& col : pvServer_->gen1CuHxrColumnNames())
    {
        EXPECT_TRUE(grp.nameExists(col)) << "Column '" << col << "' missing";
    }

    // ---- 4. Row count > 0 and multiple of 3 (kRows=3 per update) -----------
    auto tsDs = grp.openDataSet("secondsPastEpoch");
    hsize_t dims[1]{0};
    tsDs.getSpace().getSimpleExtentDims(dims);
    const hsize_t nRows = dims[0];
    ASSERT_GT(nRows, 0u) << "'CU-HXR/secondsPastEpoch' dataset is empty";
    EXPECT_EQ(nRows % 3u, 0u)
        << "Row count " << nRows << " not a multiple of 3 (kRows per update)";

    // ---- 5. No separate root-level /timestamps dataset ----------------------
    EXPECT_FALSE(file.nameExists("timestamps"))
        << "NTTable path must not create a separate /timestamps dataset";

    // ---- 6. Signal value range check ----------------------------------------
    // Gen1NTablePV fills all columns with 1.0 + sin(...) → range (0.0, 2.0].
    {
        const auto& firstCol = pvServer_->gen1CuHxrColumnNames().front();
        auto sigDs = grp.openDataSet(firstCol);
        std::vector<double> vals(static_cast<std::size_t>(nRows));
        sigDs.read(vals.data(), H5::PredType::NATIVE_DOUBLE);
        for (hsize_t i = 0; i < nRows; ++i)
        {
            EXPECT_GE(vals[i], 0.0) << "vals[" << i << "] below 0";
            EXPECT_LE(vals[i], 2.01) << "vals[" << i << "] above 2.01";
        }
    }
}

//
// Verifies that after stopping the controller the HDF5 file written from a
// live BSAS NTTable PV uses the per-column group layout.
//
//  Layout: /test:bsas_table/secondsPastEpoch  int64 [N_rows]
//          /test:bsas_table/nanoseconds       int64 [N_rows]
//          /test:bsas_table/<colName>         typed [N_rows]  (one per column)
//
//  1. File exists and is a valid HDF5 file.
//  2. A group named after the PV source exists.
//  3. No root-level /timestamps, /PV_A, /PV_B, /PV_C datasets.
//  4. Per-column datasets: PV_A, PV_B, PV_C, secondsPastEpoch, nanoseconds.
//  5. Row count is a multiple of 3 (mock emits 3 rows per event).
//  6. secondsPastEpoch values are positive.
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
    // ---- 1. Start controller and wait for data --------------------------------
    auto cfg = makeConfigFromYaml(
        buildBsasTableDetailYaml("test:bsas_table", outputDir_.string()));
    controller_ = mldp_pvxs_driver::controller::MLDPPVXSController::create(cfg);
    ASSERT_TRUE(controller_) << "Failed to create controller for test:bsas_table";
    controller_->start();

    ASSERT_FALSE(waitForActivity(std::chrono::milliseconds(5000)).empty())
        << "No .hdf5 temp file written within 5 s";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---- 2. Stop controller BEFORE opening the file --------------------------
    controller_->stop();
    controller_.reset();
    const auto h5path = waitForH5File(std::chrono::milliseconds(3000));
    ASSERT_FALSE(h5path.empty()) << "No final .hdf5 file after stop() for test:bsas_table";

    // ---- 3. Open file ---------------------------------------------------------
    H5::H5File file;
    ASSERT_NO_THROW(file = H5::H5File(h5path.string(), H5F_ACC_RDONLY))
        << "HDF5 file cannot be opened: " << h5path;

    // ---- 4. Group exists; root-level columnar datasets must NOT exist --------
    ASSERT_TRUE(file.nameExists("test:bsas_table"))
        << "Group 'test:bsas_table' missing in " << h5path;
    EXPECT_FALSE(file.nameExists("timestamps"))
        << "Unexpected /timestamps — NTTable must use per-column group layout";
    EXPECT_FALSE(file.nameExists("PV_A"))
        << "Unexpected /PV_A — NTTable must use per-column group layout";

    auto grp = file.openGroup("test:bsas_table");

    // ---- 5. Timestamp and signal datasets inside the group ------------------
    ASSERT_TRUE(grp.nameExists("secondsPastEpoch"))
        << "Dataset 'test:bsas_table/secondsPastEpoch' missing";
    ASSERT_TRUE(grp.nameExists("nanoseconds"))
        << "Dataset 'test:bsas_table/nanoseconds' missing";
    for (const char* name : {"PV_A", "PV_B", "PV_C"})
    {
        EXPECT_TRUE(grp.nameExists(name))
            << "Column '" << name << "' missing from group 'test:bsas_table'";
    }

    // ---- 6. Row count is a multiple of 3 -------------------------------------
    auto tsDs = grp.openDataSet("secondsPastEpoch");
    hsize_t dims[1]{0};
    tsDs.getSpace().getSimpleExtentDims(dims);
    ASSERT_GT(dims[0], 0u) << "'test:bsas_table/secondsPastEpoch' dataset is empty";
    EXPECT_EQ(dims[0] % 3, 0u)
        << "Row count " << dims[0] << " not multiple of 3";

    // ---- 7. secondsPastEpoch: positive ----------------------------------------
    {
        std::vector<int64_t> tsVals(static_cast<std::size_t>(dims[0]));
        tsDs.read(tsVals.data(), H5::PredType::NATIVE_INT64);
        for (hsize_t i = 0; i < dims[0]; ++i)
        {
            EXPECT_GT(tsVals[i], 0LL)
                << "secondsPastEpoch[" << i << "] = " << tsVals[i] << " not positive";
        }
    }
}

