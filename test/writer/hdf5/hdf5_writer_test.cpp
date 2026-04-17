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
        if (entry.is_regular_file() && entry.path().extension() == ".hdf5") {
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

// ---------------------------------------------------------------------------
// Helper: open the first .h5 file found under tempDir_
// ---------------------------------------------------------------------------
static fs::path findH5File(const fs::path& dir)
{
    for (const auto& entry : fs::recursive_directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".hdf5")
            return entry.path();
    }
    return {};
}

// ---------------------------------------------------------------------------
// String column tests
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterTest, StringColumnWritten)
{
    HDF5Writer w(makeConfig());
    w.start();

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000000);
    ts->set_nanoseconds(0);

    auto* col = frame.add_stringcolumns();
    col->set_name("STATUS");
    col->add_values("OK");

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:STRING";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty()) << "No .h5 file written";

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("STATUS")) << "Dataset 'STATUS' missing";

    H5::DataSet   ds = file.openDataSet("STATUS");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);

    // Read back and verify value
    H5::StrType vlStr(H5::PredType::C_S1, H5T_VARIABLE);
    std::vector<char*> buf(1, nullptr);
    ds.read(buf.data(), vlStr);
    EXPECT_STREQ(buf[0], "OK");
    H5::DataSet::vlenReclaim(buf.data(), vlStr, sp);
}

TEST_F(HDF5WriterTest, StringColumnMultipleValuesWritten)
{
    HDF5Writer w(makeConfig());
    w.start();

    // Push two frames — dataset should grow to 2 rows
    for (int i = 0; i < 2; ++i)
    {
        dp::service::common::DataFrame frame;
        auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
        ts->set_epochseconds(1700000000 + i);
        ts->set_nanoseconds(0);
        auto* col = frame.add_stringcolumns();
        col->set_name("LABEL");
        col->add_values(i == 0 ? "FIRST" : "SECOND");
        IDataBus::EventBatch batch;
        batch.root_source = "TEST:STRPV";
        batch.frames.push_back(std::move(frame));
        w.push(batch);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    H5::DataSet   ds = file.openDataSet("LABEL");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 2u);
}

// ---------------------------------------------------------------------------
// Array (waveform) column tests
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterTest, DoubleArrayColumnWrittenAs2DDataset)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int kArrayLen = 4;

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000000);
    ts->set_nanoseconds(0);

    auto* col = frame.add_doublearraycolumns();
    col->set_name("WAVEFORM");
    col->mutable_dimensions()->add_dims(kArrayLen);
    for (int i = 0; i < kArrayLen; ++i)
        col->add_values(static_cast<double>(i) * 1.5);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:WAVE";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("WAVEFORM")) << "Dataset 'WAVEFORM' missing";

    H5::DataSet   ds = file.openDataSet("WAVEFORM");
    H5::DataSpace sp = ds.getSpace();
    ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
    hsize_t dims[2]{0, 0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u)         << "Expected 1 sample (row)";
    EXPECT_EQ(dims[1], kArrayLen)  << "Expected array length " << kArrayLen;

    std::vector<double> readback(kArrayLen);
    ds.read(readback.data(), H5::PredType::NATIVE_DOUBLE);
    for (int i = 0; i < kArrayLen; ++i)
        EXPECT_DOUBLE_EQ(readback[i], static_cast<double>(i) * 1.5);
}

TEST_F(HDF5WriterTest, DoubleArrayColumnGrowsRowsAcrossUpdates)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int kArrayLen  = 3;
    constexpr int kNumFrames = 4;

    for (int f = 0; f < kNumFrames; ++f)
    {
        dp::service::common::DataFrame frame;
        auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
        ts->set_epochseconds(1700000000 + f);
        ts->set_nanoseconds(0);
        auto* col = frame.add_doublearraycolumns();
        col->set_name("WAVEFORM");
        col->mutable_dimensions()->add_dims(kArrayLen);
        for (int i = 0; i < kArrayLen; ++i)
            col->add_values(static_cast<double>(f * kArrayLen + i));
        IDataBus::EventBatch batch;
        batch.root_source = "TEST:WAVEGROW";
        batch.frames.push_back(std::move(frame));
        w.push(batch);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    H5::DataSet   ds = file.openDataSet("WAVEFORM");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[2]{0, 0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], static_cast<hsize_t>(kNumFrames));
    EXPECT_EQ(dims[1], static_cast<hsize_t>(kArrayLen));
}

TEST_F(HDF5WriterTest, FloatArrayColumnWrittenAs2DDataset)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int kArrayLen = 8;

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000000);
    ts->set_nanoseconds(0);

    auto* col = frame.add_floatarraycolumns();
    col->set_name("FLOATWAVE");
    col->mutable_dimensions()->add_dims(kArrayLen);
    for (int i = 0; i < kArrayLen; ++i)
        col->add_values(static_cast<float>(i) * 0.5f);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:FWAVE";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("FLOATWAVE"));
    H5::DataSet   ds = file.openDataSet("FLOATWAVE");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[2]{0, 0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);
    EXPECT_EQ(dims[1], static_cast<hsize_t>(kArrayLen));
}

TEST_F(HDF5WriterTest, Int32ArrayColumnWrittenAs2DDataset)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int kArrayLen = 6;

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000000);
    ts->set_nanoseconds(0);

    auto* col = frame.add_int32arraycolumns();
    col->set_name("INTWAVE");
    col->mutable_dimensions()->add_dims(kArrayLen);
    for (int i = 0; i < kArrayLen; ++i)
        col->add_values(i * 10);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:IWAVE";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("INTWAVE"));
    H5::DataSet   ds = file.openDataSet("INTWAVE");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[2]{0, 0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);
    EXPECT_EQ(dims[1], static_cast<hsize_t>(kArrayLen));

    std::vector<int32_t> readback(kArrayLen);
    ds.read(readback.data(), H5::PredType::NATIVE_INT32);
    for (int i = 0; i < kArrayLen; ++i)
        EXPECT_EQ(readback[i], i * 10);
}

// ---------------------------------------------------------------------------
// Scalar column data-value readback tests
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterTest, DoubleColumnDataReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000000);
    ts->set_nanoseconds(500000000);

    auto* col = frame.add_doublecolumns();
    col->set_name("VOLTAGE");
    col->add_values(1.23);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:DBLREADBACK";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("VOLTAGE"));

    H5::DataSet      ds = file.openDataSet("VOLTAGE");
    std::vector<double> readback(1);
    ds.read(readback.data(), H5::PredType::NATIVE_DOUBLE);
    EXPECT_DOUBLE_EQ(readback[0], 1.23);
}

TEST_F(HDF5WriterTest, FloatColumnWrittenAndReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000001);
    ts->set_nanoseconds(0);

    auto* col = frame.add_floatcolumns();
    col->set_name("TEMP");
    col->add_values(3.14f);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:FLTREADBACK";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("TEMP"));

    H5::DataSet     ds = file.openDataSet("TEMP");
    H5::DataSpace   sp = ds.getSpace();
    hsize_t         dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);

    std::vector<float> readback(1);
    ds.read(readback.data(), H5::PredType::NATIVE_FLOAT);
    EXPECT_FLOAT_EQ(readback[0], 3.14f);
}

TEST_F(HDF5WriterTest, Int32ColumnWrittenAndReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000002);
    ts->set_nanoseconds(0);

    auto* col = frame.add_int32columns();
    col->set_name("COUNTER");
    col->add_values(42);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:I32READBACK";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("COUNTER"));

    H5::DataSet      ds = file.openDataSet("COUNTER");
    H5::DataSpace    sp = ds.getSpace();
    hsize_t          dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);

    std::vector<int32_t> readback(1);
    ds.read(readback.data(), H5::PredType::NATIVE_INT32);
    EXPECT_EQ(readback[0], 42);
}

TEST_F(HDF5WriterTest, Int64ColumnWrittenAndReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000003);
    ts->set_nanoseconds(0);

    auto* col = frame.add_int64columns();
    col->set_name("BIGVAL");
    col->add_values(9876543210LL);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:I64READBACK";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("BIGVAL"));

    H5::DataSet      ds = file.openDataSet("BIGVAL");
    H5::DataSpace    sp = ds.getSpace();
    hsize_t          dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);

    std::vector<int64_t> readback(1);
    ds.read(readback.data(), H5::PredType::NATIVE_INT64);
    EXPECT_EQ(readback[0], 9876543210LL);
}

TEST_F(HDF5WriterTest, BoolColumnWrittenAndReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000004);
    ts->set_nanoseconds(0);

    auto* col = frame.add_boolcolumns();
    col->set_name("ENABLED");
    col->add_values(true);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:BOOLREADBACK";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("ENABLED"));

    H5::DataSet      ds = file.openDataSet("ENABLED");
    H5::DataSpace    sp = ds.getSpace();
    hsize_t          dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);

    std::vector<unsigned int> readback(1, 0u);
    ds.read(readback.data(), H5::PredType::NATIVE_HBOOL);
    EXPECT_EQ(readback[0], 1u);
}

TEST_F(HDF5WriterTest, TimestampDatasetWrittenAndReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int64_t kEpochSec = 1700000005;
    constexpr int64_t kNanos    = 123456789;

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(kEpochSec);
    ts->set_nanoseconds(static_cast<uint32_t>(kNanos));

    auto* col = frame.add_doublecolumns();
    col->set_name("DUMMY");
    col->add_values(0.0);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:TSREADBACK";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("timestamps"));

    H5::DataSet   ds = file.openDataSet("timestamps");
    H5::DataSpace sp = ds.getSpace();
    hsize_t       dims[1]{0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);

    std::vector<int64_t> readback(1);
    ds.read(readback.data(), H5::PredType::NATIVE_INT64);
    const int64_t expected = kEpochSec * 1'000'000'000LL + kNanos;
    EXPECT_EQ(readback[0], expected);
}

// ---------------------------------------------------------------------------
// Float array column value readback
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterTest, FloatArrayColumnValuesReadBack)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int kArrayLen = 4;

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000006);
    ts->set_nanoseconds(0);

    auto* col = frame.add_floatarraycolumns();
    col->set_name("FWAVE_VALS");
    col->mutable_dimensions()->add_dims(kArrayLen);
    for (int i = 0; i < kArrayLen; ++i)
        col->add_values(static_cast<float>(i) * 2.5f);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:FWAVEVALS";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("FWAVE_VALS"));
    H5::DataSet   ds = file.openDataSet("FWAVE_VALS");
    H5::DataSpace sp = ds.getSpace();
    ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
    hsize_t dims[2]{0, 0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);
    EXPECT_EQ(dims[1], static_cast<hsize_t>(kArrayLen));

    std::vector<float> readback(kArrayLen);
    ds.read(readback.data(), H5::PredType::NATIVE_FLOAT);
    for (int i = 0; i < kArrayLen; ++i)
        EXPECT_FLOAT_EQ(readback[i], static_cast<float>(i) * 2.5f);
}

// ---------------------------------------------------------------------------
// Int64 array column (no prior test existed)
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterTest, Int64ArrayColumnWrittenAs2DDataset)
{
    HDF5Writer w(makeConfig());
    w.start();

    constexpr int kArrayLen = 5;

    dp::service::common::DataFrame frame;
    auto* ts = frame.mutable_datatimestamps()->mutable_timestamplist()->add_timestamps();
    ts->set_epochseconds(1700000007);
    ts->set_nanoseconds(0);

    auto* col = frame.add_int64arraycolumns();
    col->set_name("BIGWAVE");
    col->mutable_dimensions()->add_dims(kArrayLen);
    for (int i = 0; i < kArrayLen; ++i)
        col->add_values(static_cast<int64_t>(i) * 1000000000LL);

    IDataBus::EventBatch batch;
    batch.root_source = "TEST:I64WAVE";
    batch.frames.push_back(std::move(frame));
    w.push(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    w.stop();

    auto h5path = findH5File(tempDir_);
    ASSERT_FALSE(h5path.empty());

    H5::H5File    file(h5path.string(), H5F_ACC_RDONLY);
    ASSERT_TRUE(file.nameExists("BIGWAVE"));
    H5::DataSet   ds = file.openDataSet("BIGWAVE");
    H5::DataSpace sp = ds.getSpace();
    ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
    hsize_t dims[2]{0, 0};
    sp.getSimpleExtentDims(dims);
    EXPECT_EQ(dims[0], 1u);
    EXPECT_EQ(dims[1], static_cast<hsize_t>(kArrayLen));

    std::vector<int64_t> readback(kArrayLen);
    ds.read(readback.data(), H5::PredType::NATIVE_INT64);
    for (int i = 0; i < kArrayLen; ++i)
        EXPECT_EQ(readback[i], static_cast<int64_t>(i) * 1000000000LL);
}

#endif // MLDP_PVXS_HDF5_ENABLED
