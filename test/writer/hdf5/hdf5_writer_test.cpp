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

#ifdef MLDP_PVXS_HDF5_ENABLED

#include <writer/hdf5/HDF5Writer.h>
#include <writer/hdf5/HDF5WriterConfig.h>
#include <writer/WriterFactory.h>
#include <util/bus/IDataBus.h>
#include <common.pb.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "../../config/test_config_helpers.h"

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::bus;
using mldp_pvxs_driver::config::makeConfigFromYaml;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class HDF5WriterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tempDir_ = fs::temp_directory_path() / "hdf5_writer_test";
        fs::create_directories(tempDir_);
    }

    void TearDown() override { fs::remove_all(tempDir_); }

    HDF5WriterConfig makeConfig()
    {
        HDF5WriterConfig cfg;
        cfg.basePath      = tempDir_.string();
        cfg.name          = "test_writer";
        cfg.flushInterval = std::chrono::milliseconds(100);
        return cfg;
    }

    /// Build a minimal EventBatch that passes the DataFrame timestamp check.
    static IDataBus::EventBatch makeValidBatch()
    {
        dp::service::common::DataFrame frame;

        auto* timestamps = frame.mutable_datatimestamps()->mutable_timestamplist();
        auto* ts         = timestamps->add_timestamps();
        ts->set_epochseconds(1700000000);
        ts->set_nanoseconds(0);

        auto* col = frame.add_doublecolumns();
        col->set_name("VOLTAGE");
        col->add_values(1.23);

        IDataBus::EventBatch batch;
        batch.root_source = "TEST:PV";
        batch.frames.push_back(std::move(frame));
        return batch;
    }

    fs::path tempDir_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterTest, StartAndStopDoNotThrow)
{
    HDF5Writer w(makeConfig());
    EXPECT_NO_THROW(w.start());
    EXPECT_NO_THROW(w.stop());
}

TEST_F(HDF5WriterTest, NameReturnsConfiguredName)
{
    HDF5Writer w(makeConfig());
    EXPECT_EQ(w.name(), "test_writer");
}

TEST_F(HDF5WriterTest, PushReturnsFalseWhenNotStarted)
{
    HDF5Writer w(makeConfig());
    w.start();
    w.stop();
    // After stop() the writer is no longer accepting data.
    auto batch = makeValidBatch();
    EXPECT_FALSE(w.push(batch));
}

TEST_F(HDF5WriterTest, PushReturnsTrueWhenRunning)
{
    HDF5Writer w(makeConfig());
    w.start();
    auto batch = makeValidBatch();
    EXPECT_TRUE(w.push(batch));
    w.stop();
}

TEST_F(HDF5WriterTest, PushWritesFileToBasePath)
{
    HDF5Writer w(makeConfig());
    w.start();

    auto batch = makeValidBatch();
    w.push(batch);

    // Give the writer thread time to flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    w.stop();

    bool found = false;
    for (const auto& entry : fs::recursive_directory_iterator(tempDir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".h5") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected at least one .h5 file under " << tempDir_;
}

TEST_F(HDF5WriterTest, WriterFactoryCreatesHDF5Writer)
{
    auto node = makeConfigFromYaml(
        "name: factory_writer\n"
        "base-path: " + tempDir_.string() + "\n"
        "flush-interval-ms: 100\n"
    );

    auto w = WriterFactory::create("hdf5", node, nullptr);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->name(), "factory_writer");
    w->start();
    w->stop();
}

#endif // MLDP_PVXS_HDF5_ENABLED
