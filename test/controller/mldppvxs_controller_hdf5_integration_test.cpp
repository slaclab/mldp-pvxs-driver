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
                if (e.is_regular_file() && e.path().extension() == ".h5")
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
// NTTable test
// ---------------------------------------------------------------------------

TEST_F(ControllerHDF5Test, NTTablePressureWritesDatasets)
{
    startController("test:table");
    const auto h5path = waitForH5File();
    ASSERT_FALSE(h5path.empty()) << "No .h5 file written for test:table";
    controller_->stop();
    controller_.reset();

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("timestamps")) << "No 'timestamps' dataset in " << h5path;
    hsize_t       dims[1]{0};
    H5::DataSet   tsDs = file.openDataSet("timestamps");
    H5::DataSpace tsSp = tsDs.getSpace();
    tsSp.getSimpleExtentDims(dims);
    EXPECT_GT(dims[0], 0u) << "'timestamps' dataset is empty";

    const bool hasPressure  = file.nameExists("pressure");
    const bool hasDeviceIDs = file.nameExists("deviceIDs");
    EXPECT_TRUE(hasPressure || hasDeviceIDs)
        << "Expected at least one column dataset (pressure or deviceIDs) in " << h5path;
}
