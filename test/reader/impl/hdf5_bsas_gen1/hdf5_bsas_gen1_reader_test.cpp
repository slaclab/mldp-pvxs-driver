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

    #include <reader/impl/hdf5_bsas_gen1/HDF5BsasGen1Reader.h>
    #include <reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.h>
    #include <util/bus/IDataBus.h>

    #include <H5Cpp.h>

    #include <algorithm>
    #include <array>
    #include <cctype>
    #include <chrono>
    #include <cmath>
    #include <cstdlib>
    #include <cstring>
    #include <filesystem>
    #include <iomanip>
    #include <sstream>
    #include <thread>

    #include "config/test_config_helpers.h"
    #include "mock/BsasGen1HDF5Mock.h"
    #include "mock/MockDataBus.h"

using namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::test::mock;
using mldp_pvxs_driver::config::makeConfigFromYaml;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class HDF5BsasGen1ReaderTest : public ::testing::Test
{
protected:
    static constexpr std::size_t kFloatCols = 10;
    static constexpr std::size_t kIntCols = 4;
    static constexpr std::size_t kRows = 20;
    static constexpr uint32_t    kBaseEpoch = 1700000000u;

    void SetUp() override
    {
        tempDir_ = fs::path(MLDP_TEST_DATA_DIR) / "hdf5";
        fs::create_directories(tempDir_);
        mockFile_ = (tempDir_ / "mock_bsas_gen1.h5").string();

        BsasGen1HDF5Mock::Params params;
        params.numFloatCols = kFloatCols;
        params.numIntCols = kIntCols;
        params.numRows = kRows;
        params.baseEpoch = kBaseEpoch;
        BsasGen1HDF5Mock::generate(mockFile_, params);
    }

    void TearDown() override
    {
        fs::remove(mockFile_);
    }

    fs::path    tempDir_;
    std::string mockFile_;
};

// ---------------------------------------------------------------------------
// Config tests
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, ConfigParsesValidYaml)
{
    auto cfg = makeConfigFromYaml(
        "name: test_reader\n" "file-path: " + mockFile_ + "\n" "chunk-size: 5\n");

    HDF5BsasGen1ReaderConfig config(cfg);
    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.name(), "test_reader");
    EXPECT_EQ(config.filePath(), mockFile_);
    EXPECT_EQ(config.chunkSize(), 5u);
}

TEST_F(HDF5BsasGen1ReaderTest, ConfigDefaultChunkSize)
{
    auto cfg = makeConfigFromYaml(
        "name: test_reader\n" "file-path: " + mockFile_ + "\n");

    HDF5BsasGen1ReaderConfig config(cfg);
    EXPECT_EQ(config.chunkSize(), 1000u);
}

TEST_F(HDF5BsasGen1ReaderTest, ConfigThrowsOnMissingFilePath)
{
    auto cfg = makeConfigFromYaml("name: no_path\n");
    EXPECT_THROW(HDF5BsasGen1ReaderConfig{cfg}, HDF5BsasGen1ReaderConfig::Error);
}

TEST_F(HDF5BsasGen1ReaderTest, ConfigThrowsOnMissingName)
{
    auto cfg = makeConfigFromYaml("file-path: /tmp/test.h5\n");
    EXPECT_THROW(HDF5BsasGen1ReaderConfig{cfg}, HDF5BsasGen1ReaderConfig::Error);
}

// ---------------------------------------------------------------------------
// Reader integration tests
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, ReaderEmitsBatches)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_test\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    // With 20 rows and chunk-size 1000, should get 1 data batch + 1 marker = 2
    ASSERT_EQ(batches.size(), 2u);

    // First batch is the data batch (tabular, with frames)
    ASSERT_TRUE(isTimeSeries(batches[0]));
    const auto& tsp = asTimeSeries(batches[0]);
    EXPECT_TRUE(tsp.is_tabular);
    EXPECT_FALSE(tsp.end_of_batch_group);
    EXPECT_EQ(tsp.frames.size(), kFloatCols + kIntCols);

    // Second batch is the end_of_batch_group marker
    ASSERT_TRUE(isTimeSeries(batches[1]));
    const auto& marker = asTimeSeries(batches[1]);
    EXPECT_TRUE(marker.end_of_batch_group);
    EXPECT_TRUE(marker.is_tabular);
}

TEST_F(HDF5BsasGen1ReaderTest, ReaderExpandsGlobPatternsFromConfiguredPath)
{
    const auto secondFile = (tempDir_ / "second_bsas_gen1.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numFloatCols = kFloatCols;
    params.numIntCols = kIntCols;
    params.numRows = 7;
    params.baseEpoch = kBaseEpoch + 1000;
    BsasGen1HDF5Mock::generate(secondFile, params);

    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_glob\n" "file-path: " + (tempDir_ / "*.h5").string() + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_EQ(batches.size(), 4u);

    ASSERT_TRUE(isTimeSeries(batches[0]));
    ASSERT_TRUE(isTimeSeries(batches[2]));
    EXPECT_EQ(asTimeSeries(batches[0]).frames[0].timestamps.size(), kRows);
    EXPECT_EQ(asTimeSeries(batches[2]).frames[0].timestamps.size(), 7u);

    fs::remove(secondFile);
}

TEST_F(HDF5BsasGen1ReaderTest, ChunkedReadingProducesMultipleBatches)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_chunked\n" "file-path: " + mockFile_ + "\n" "chunk-size: 7\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    // 20 rows / 7 chunk = 3 chunks (7+7+6). Each chunk = 1 data + 1 marker = 2.
    // Total = 6 batches
    EXPECT_EQ(batches.size(), 6u);
}

TEST_F(HDF5BsasGen1ReaderTest, TimestampsAreCorrect)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_ts\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_FALSE(tsp.frames.empty());

    const auto& frame = tsp.frames[0];
    ASSERT_EQ(frame.timestamps.size(), kRows);

    for (std::size_t r = 0; r < kRows; ++r)
    {
        EXPECT_EQ(frame.timestamps[r].epoch_seconds, kBaseEpoch + r);
        EXPECT_EQ(frame.timestamps[r].nanoseconds, r * 1000u);
    }
}

TEST_F(HDF5BsasGen1ReaderTest, Float64ColumnValuesAreCorrect)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_float\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_GE(tsp.frames.size(), kFloatCols);

    // Check first float64 column (col index 0)
    const auto& frame = tsp.frames[0];
    ASSERT_EQ(frame.columns.size(), 1u);
    EXPECT_EQ(frame.columns[0].name, "SIG_0000");

    const auto& vals = std::get<std::vector<double>>(frame.columns[0].values);
    ASSERT_EQ(vals.size(), kRows);

    for (std::size_t r = 0; r < kRows; ++r)
    {
        double expected = std::sin(static_cast<double>(r) * 0.1 + 0.0 * 0.01);
        EXPECT_NEAR(vals[r], expected, 1e-10) << "row " << r;
    }
}

TEST_F(HDF5BsasGen1ReaderTest, Int16ColumnValuesAreCorrectAsInt32)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_int\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    // Int columns come after float columns
    ASSERT_GE(tsp.frames.size(), kFloatCols + 1);

    const auto& frame = tsp.frames[kFloatCols]; // first int column
    ASSERT_EQ(frame.columns.size(), 1u);
    EXPECT_EQ(frame.columns[0].name, "FLAG_00");

    const auto& vals = std::get<std::vector<int32_t>>(frame.columns[0].values);
    ASSERT_EQ(vals.size(), kRows);

    for (std::size_t r = 0; r < kRows; ++r)
    {
        int32_t expected = static_cast<int32_t>(r + 0); // row + col_idx(0)
        EXPECT_EQ(vals[r], expected) << "row " << r;
    }
}

TEST_F(HDF5BsasGen1ReaderTest, ColumnNamesMatchDatasetNames)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_names\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_EQ(tsp.frames.size(), kFloatCols + kIntCols);

    // Verify all float column names
    for (std::size_t c = 0; c < kFloatCols; ++c)
    {
        ASSERT_EQ(tsp.frames[c].columns.size(), 1u);
        std::ostringstream expected;
        expected << "SIG_" << std::setfill('0') << std::setw(4) << c;
        EXPECT_EQ(tsp.frames[c].columns[0].name, expected.str());
    }

    // Verify all int column names
    for (std::size_t c = 0; c < kIntCols; ++c)
    {
        ASSERT_EQ(tsp.frames[kFloatCols + c].columns.size(), 1u);
        std::ostringstream expected;
        expected << "FLAG_" << std::setfill('0') << std::setw(2) << c;
        EXPECT_EQ(tsp.frames[kFloatCols + c].columns[0].name, expected.str());
    }
}

TEST_F(HDF5BsasGen1ReaderTest, ColumnNamesAlwaysUseDatasetName)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_dsname\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_GE(tsp.frames.size(), 1u);

    // Column name is always the HDF5 dataset name, not the label
    EXPECT_EQ(tsp.frames[0].columns[0].name, "SIG_0000");
}

TEST_F(HDF5BsasGen1ReaderTest, InvalidUtf8LabelDoesNotAffectColumnName)
{
    const auto invalidFile = (tempDir_ / "mock_bsas_gen1_invalid_label.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numFloatCols = kFloatCols;
    params.numIntCols = kIntCols;
    params.numRows = kRows;
    params.baseEpoch = kBaseEpoch;
    params.injectInvalidUtf8Label = true;
    BsasGen1HDF5Mock::generate(invalidFile, params);

    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_label_invalid\n" "file-path: " + invalidFile + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_GE(tsp.frames.size(), 1u);
    // Dataset name is always used regardless of label validity
    EXPECT_EQ(tsp.frames[0].columns[0].name, "SIG_0000");

    fs::remove(invalidFile);
}

TEST_F(HDF5BsasGen1ReaderTest, MatlabStyleDatasetNamesArePreserved)
{
    const auto matlabLikeFile = (tempDir_ / "mock_bsas_matlab_style.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numRows = kRows;
    params.baseEpoch = kBaseEpoch;
    params.explicitColumns = std::vector<BsasGen1HDF5Mock::Params::ColumnSpec>{
        {"ACCL_IN20_300_L0A_ACUSBR", "ACCL:IN20:300:L0A_ACUSBR", true},
        {"ACCL_IN20_300_L0A_PCUSBR", "ACCL:IN20:300:L0A_PCUSBR", true},
        {"TORO_IN20_791_TMITCUSBR", "TORO:IN20:791:TMITCUSBR", true},
        {"WIRE_IN20_561_POSNCUSBR", "WIRE:IN20:561:POSNCUSBR", false},
        {"DUMP_LI21_305_TGT_STS", "DUMP:LI21:305:TGT:STS", false},
    };
    BsasGen1HDF5Mock::generate(matlabLikeFile, params);

    // Column names come from dataset names (float64 sorted first, then int16)
    const std::vector<std::string> expectedNames = {
        "ACCL_IN20_300_L0A_ACUSBR",
        "ACCL_IN20_300_L0A_PCUSBR",
        "TORO_IN20_791_TMITCUSBR",
        "DUMP_LI21_305_TGT_STS",
        "WIRE_IN20_561_POSNCUSBR",
    };

    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_real\n" "file-path: " + matlabLikeFile + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_EQ(tsp.frames.size(), expectedNames.size());

    for (std::size_t i = 0; i < expectedNames.size(); ++i)
    {
        ASSERT_EQ(tsp.frames[i].columns.size(), 1u) << "frame " << i;
        EXPECT_EQ(tsp.frames[i].columns[0].name, expectedNames[i]) << "frame " << i;
    }

    fs::remove(matlabLikeFile);
}

TEST_F(HDF5BsasGen1ReaderTest, ReaderNameMatches)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: my_reader\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    HDF5BsasGen1Reader reader(bus, nullptr, cfg);
    EXPECT_EQ(reader.name(), "my_reader");
}

// ---------------------------------------------------------------------------
// Mock structure validation — verify generated file matches flat format
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, MockFileMatchesFlatStructure)
{
    H5::H5File file(mockFile_, H5F_ACC_RDONLY);

    // No root attributes
    EXPECT_EQ(file.getNumAttrs(), 0u);

    // No groups — only datasets at root
    hsize_t numObjs = file.getNumObjs();
    EXPECT_EQ(numObjs, kFloatCols + kIntCols + 2); // +2 for timestamps

    for (hsize_t i = 0; i < numObjs; ++i)
        EXPECT_EQ(file.getObjTypeByIdx(i), H5G_DATASET);

    // Verify float64 dataset structure
    {
        H5::DataSet   ds = file.openDataSet("SIG_0000");
        H5::DataSpace sp = ds.getSpace();
        ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
        hsize_t dims[2]{0, 0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(dims[1], 1u);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_FLOAT);
        EXPECT_EQ(ds.getDataType().getSize(), 8u);
        ASSERT_TRUE(ds.attrExists("MATLAB_class"));
        ASSERT_TRUE(ds.attrExists("label"));
    }

    // Verify int16 dataset structure
    {
        H5::DataSet   ds = file.openDataSet("FLAG_00");
        H5::DataSpace sp = ds.getSpace();
        hsize_t       dims[2]{0, 0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(dims[1], 1u);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_INTEGER);
        EXPECT_EQ(ds.getDataType().getSize(), 2u);
        ASSERT_TRUE(ds.attrExists("MATLAB_class"));
        ASSERT_TRUE(ds.attrExists("label"));
    }

    // Verify timestamp datasets
    {
        H5::DataSet ds = file.openDataSet("secondsPastEpoch");
        EXPECT_EQ(ds.getDataType().getClass(), H5T_INTEGER);
        EXPECT_EQ(ds.getDataType().getSize(), 4u);
        hsize_t dims[2]{0, 0};
        ds.getSpace().getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(dims[1], 1u);
    }
    {
        H5::DataSet ds = file.openDataSet("nanoseconds");
        EXPECT_EQ(ds.getDataType().getClass(), H5T_INTEGER);
        EXPECT_EQ(ds.getDataType().getSize(), 4u);
    }
}

// ---------------------------------------------------------------------------
// Large-scale reader test — env-configurable row/column count.
// Verifies every single row of every column is emitted correctly.
//
// Environment variables:
//   BSAS_GEN1_TEST_FLOAT_COLS — number of float64 columns (default: 100)
//   BSAS_GEN1_TEST_INT_COLS   — number of int16 columns (default: 16)
//   BSAS_GEN1_TEST_ROWS       — number of rows (default: 5000)
//   BSAS_GEN1_TEST_CHUNK_SIZE — reader chunk size (default: 512)
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, LargeScaleReaderEmitsAllData)
{
    const char* envFloat = std::getenv("BSAS_GEN1_TEST_FLOAT_COLS");
    const char* envInt = std::getenv("BSAS_GEN1_TEST_INT_COLS");
    const char* envRows = std::getenv("BSAS_GEN1_TEST_ROWS");
    const char* envChunk = std::getenv("BSAS_GEN1_TEST_CHUNK_SIZE");

    const std::size_t  numFloatCols = envFloat ? static_cast<std::size_t>(std::atol(envFloat)) : 100;
    const std::size_t  numIntCols = envInt ? static_cast<std::size_t>(std::atol(envInt)) : 16;
    const std::size_t  numRows = envRows ? static_cast<std::size_t>(std::atol(envRows)) : 5000;
    const std::size_t  chunkSize = envChunk ? static_cast<std::size_t>(std::atol(envChunk)) : 512;
    constexpr uint32_t baseEpoch = 1700000000u;

    const std::string largeFile = (tempDir_ / "mock_bsas_large.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numFloatCols = numFloatCols;
    params.numIntCols = numIntCols;
    params.numRows = numRows;
    params.baseEpoch = baseEpoch;
    BsasGen1HDF5Mock::generate(largeFile, params);

    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_large\n" "file-path: " + largeFile + "\n" "chunk-size: " + std::to_string(chunkSize) + "\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        // Wait long enough for large file processing
        const auto waitMs = std::max(2000, static_cast<int>(numRows * (numFloatCols + numIntCols) / 5000));
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    }

    auto batches = bus->snapshot();

    // Calculate expected chunks
    const std::size_t expectedChunks = (numRows + chunkSize - 1) / chunkSize;
    // Each chunk = 1 data batch + 1 marker = 2 pushes
    ASSERT_EQ(batches.size(), expectedChunks * 2)
        << "Expected " << expectedChunks << " chunks × 2 batches each";

    // Verify all emitted data matches the generated values exactly
    std::size_t rowOffset = 0;
    for (std::size_t chunk = 0; chunk < expectedChunks; ++chunk)
    {
        const std::size_t batchIdx = chunk * 2;
        ASSERT_TRUE(isTimeSeries(batches[batchIdx])) << "chunk " << chunk;
        const auto& tsp = asTimeSeries(batches[batchIdx]);
        EXPECT_TRUE(tsp.is_tabular);
        EXPECT_FALSE(tsp.end_of_batch_group);

        const std::size_t chunkRows = std::min(chunkSize, numRows - rowOffset);
        ASSERT_EQ(tsp.frames.size(), numFloatCols + numIntCols)
            << "chunk " << chunk << " frame count";

        // Verify float64 columns
        for (std::size_t c = 0; c < numFloatCols; ++c)
        {
            const auto& frame = tsp.frames[c];
            ASSERT_EQ(frame.timestamps.size(), chunkRows);
            ASSERT_EQ(frame.columns.size(), 1u);
            const auto& vals = std::get<std::vector<double>>(frame.columns[0].values);
            ASSERT_EQ(vals.size(), chunkRows);

            for (std::size_t r = 0; r < chunkRows; ++r)
            {
                const std::size_t globalRow = rowOffset + r;
                // Verify timestamp
                EXPECT_EQ(frame.timestamps[r].epoch_seconds, baseEpoch + globalRow)
                    << "chunk=" << chunk << " col=" << c << " row=" << r;
                EXPECT_EQ(frame.timestamps[r].nanoseconds, static_cast<uint64_t>(globalRow * 1000))
                    << "chunk=" << chunk << " col=" << c << " row=" << r;
                // Verify value
                double expected = std::sin(static_cast<double>(globalRow) * 0.1 +
                                           static_cast<double>(c) * 0.01);
                EXPECT_NEAR(vals[r], expected, 1e-10)
                    << "chunk=" << chunk << " col=" << c << " row=" << r;
            }
        }

        // Verify int16 columns (stored as int32)
        for (std::size_t c = 0; c < numIntCols; ++c)
        {
            const auto& frame = tsp.frames[numFloatCols + c];
            ASSERT_EQ(frame.columns.size(), 1u);
            const auto& vals = std::get<std::vector<int32_t>>(frame.columns[0].values);
            ASSERT_EQ(vals.size(), chunkRows);

            for (std::size_t r = 0; r < chunkRows; ++r)
            {
                const std::size_t globalRow = rowOffset + r;
                int32_t           expected = static_cast<int32_t>(globalRow + c);
                EXPECT_EQ(vals[r], expected)
                    << "chunk=" << chunk << " intcol=" << c << " row=" << r;
            }
        }

        // Verify marker
        const auto& marker = asTimeSeries(batches[batchIdx + 1]);
        EXPECT_TRUE(marker.end_of_batch_group);
        EXPECT_TRUE(marker.is_tabular);

        rowOffset += chunkRows;
    }

    EXPECT_EQ(rowOffset, numRows) << "Total rows emitted must equal input rows";

    fs::remove(largeFile);
}

// ---------------------------------------------------------------------------
// Provenance configuration tests
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, ProvenanceFlowsToEventBatch)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_prov\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n" "provenance:\n" "  facility: LCLS\n" "  instrument: CXI\n" "  subsystem: BSAS\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    EXPECT_EQ(batches[0].metadata.at("facility"), "LCLS");
    EXPECT_EQ(batches[0].metadata.at("instrument"), "CXI");
    EXPECT_EQ(batches[0].metadata.at("subsystem"), "BSAS");
}

TEST_F(HDF5BsasGen1ReaderTest, MissingProvenanceIsValid)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_noprov\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    EXPECT_EQ(batches[0].metadata.count("facility"), 0u);
    EXPECT_EQ(batches[0].metadata.count("instrument"), 0u);
    EXPECT_EQ(batches[0].metadata.count("subsystem"), 0u);
}

// ---------------------------------------------------------------------------
// shardSlot metadata tests
// ---------------------------------------------------------------------------

// Helper: collect all shardSlot strings from the first data batch.
static std::vector<std::string> collectShardSlots(const IDataBus::EventBatch& batch)
{
    std::vector<std::string> slots;
    if (!isTimeSeries(batch))
        return slots;
    for (const auto& frame : asTimeSeries(batch).frames)
        for (const auto& col : frame.columns)
        {
            auto it = col.metadata.find("shardSlot");
            if (it != col.metadata.end())
                slots.push_back(it->second);
        }
    return slots;
}

// shardSlot present on every column in every data batch.
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotPresentOnAllColumns)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_present\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto& tsp = asTimeSeries(batches[0]);
    ASSERT_EQ(tsp.frames.size(), kFloatCols + kIntCols);

    for (std::size_t i = 0; i < tsp.frames.size(); ++i)
    {
        ASSERT_EQ(tsp.frames[i].columns.size(), 1u) << "frame " << i;
        EXPECT_TRUE(tsp.frames[i].columns[0].metadata.count("shardSlot") > 0)
            << "frame " << i << " missing shardSlot";
    }
}

// shardSlot is a 5-digit zero-padded decimal string in [00000, 65535].
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotIsZeroPaddedFiveDigitDecimal)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_format\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto slots = collectShardSlots(batches[0]);
    ASSERT_FALSE(slots.empty());

    for (const auto& s : slots)
    {
        ASSERT_EQ(s.size(), 5u) << "slot '" << s << "' not 5 chars";
        for (char c : s)
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(c))) << "non-digit in '" << s << "'";
        const int v = std::stoi(s);
        EXPECT_GE(v, 0);
        EXPECT_LE(v, 65535);
    }
}

// shardSlot values span all 6 shard ranges (round-robin distribution).
// With kFloatCols=10 + kIntCols=4 = 14 columns and 6 shards, each of the 6
// shard ranges [00000-10921], [10922-21843], … must contain at least one slot.
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotCoverAllSixShardRanges)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_dist\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto slots = collectShardSlots(batches[0]);
    ASSERT_EQ(slots.size(), kFloatCols + kIntCols);

    // 6 shards, 65536 slots → each range is 10922 slots wide (last may be 10926)
    constexpr int numShards = 6;
    constexpr int shardSize = 65536 / numShards;
    std::array<bool, numShards> covered{};
    covered.fill(false);

    for (const auto& s : slots)
    {
        const int v = std::stoi(s);
        const int shard = v / shardSize;
        ASSERT_GE(shard, 0);
        ASSERT_LT(shard, numShards) << "slot " << v << " out of range";
        covered[static_cast<std::size_t>(shard)] = true;
    }

    // With 14 columns round-robin over 6 shards, all 6 must be hit
    for (int i = 0; i < numShards; ++i)
        EXPECT_TRUE(covered[static_cast<std::size_t>(i)]) << "shard " << i << " has no column";
}

// Columns are assigned strictly round-robin: col[0]→shard0, col[1]→shard1, …
// Verify by checking each column's slot falls in the expected shard range.
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotAssignmentIsRoundRobin)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_rr\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto slots = collectShardSlots(batches[0]);
    ASSERT_EQ(slots.size(), kFloatCols + kIntCols);

    constexpr int numShards = 6;
    constexpr int shardSize = 65536 / numShards;

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        const int v = std::stoi(slots[i]);
        const int expectedShard = static_cast<int>(i % static_cast<std::size_t>(numShards));
        const int lo = expectedShard * shardSize;
        const int hi = lo + shardSize - 1;
        EXPECT_GE(v, lo) << "col " << i << " slot " << v << " below shard " << expectedShard << " range";
        EXPECT_LE(v, hi) << "col " << i << " slot " << v << " above shard " << expectedShard << " range";
    }
}

// Same column name always maps to the same shardSlot across multiple files
// processed in a single reader run (pv_shard_slot_map_ persistent across files).
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotIsStableAcrossFiles)
{
    const auto secondFile = (tempDir_ / "mock_bsas_stable_shard.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numFloatCols = kFloatCols;
    params.numIntCols = kIntCols;
    params.numRows = kRows;
    params.baseEpoch = kBaseEpoch + 1000;
    BsasGen1HDF5Mock::generate(secondFile, params);

    auto bus = std::make_shared<MockDataBus>();
    // Glob both files — reader processes them sequentially in one run
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_stable\n" "file-path: " + (tempDir_ / "*.h5").string() + "\n" "chunk-size: 1000\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    // 2 files × (1 data + 1 marker) = 4 batches
    ASSERT_EQ(batches.size(), 4u);

    // Collect slots from file 1 (batch 0) and file 2 (batch 2)
    const auto slotsFile1 = collectShardSlots(batches[0]);
    const auto slotsFile2 = collectShardSlots(batches[2]);

    ASSERT_EQ(slotsFile1.size(), kFloatCols + kIntCols);
    ASSERT_EQ(slotsFile2.size(), kFloatCols + kIntCols);

    // Both files have identical column names → identical slots
    for (std::size_t i = 0; i < slotsFile1.size(); ++i)
        EXPECT_EQ(slotsFile1[i], slotsFile2[i])
            << "column " << i << " got different shardSlot across files";

    fs::remove(secondFile);
}

// shardSlot is stable across chunks of the same file (all chunks same slot per column).
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotIsStableAcrossChunks)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_chunks\n" "file-path: " + mockFile_ + "\n" "chunk-size: 7\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    // 20 rows / 7 = 3 chunks → 6 batches (data+marker each)
    ASSERT_EQ(batches.size(), 6u);

    const auto slotsChunk0 = collectShardSlots(batches[0]);
    const auto slotsChunk1 = collectShardSlots(batches[2]);
    const auto slotsChunk2 = collectShardSlots(batches[4]);

    ASSERT_EQ(slotsChunk0.size(), kFloatCols + kIntCols);
    ASSERT_EQ(slotsChunk1.size(), kFloatCols + kIntCols);
    ASSERT_EQ(slotsChunk2.size(), kFloatCols + kIntCols);

    for (std::size_t i = 0; i < slotsChunk0.size(); ++i)
    {
        EXPECT_EQ(slotsChunk0[i], slotsChunk1[i]) << "column " << i << " chunk 0 vs 1";
        EXPECT_EQ(slotsChunk0[i], slotsChunk2[i]) << "column " << i << " chunk 0 vs 2";
    }
}

// num-shards config: default is 6.
TEST_F(HDF5BsasGen1ReaderTest, ConfigDefaultNumShardsIsSix)
{
    auto cfg = makeConfigFromYaml(
        "name: test_reader\n" "file-path: " + mockFile_ + "\n");

    HDF5BsasGen1ReaderConfig config(cfg);
    EXPECT_EQ(config.numShards(), 6u);
}

// num-shards config: explicit value parsed correctly.
TEST_F(HDF5BsasGen1ReaderTest, ConfigNumShardsCustomValue)
{
    auto cfg = makeConfigFromYaml(
        "name: test_reader\n" "file-path: " + mockFile_ + "\n" "num-shards: 3\n");

    HDF5BsasGen1ReaderConfig config(cfg);
    EXPECT_EQ(config.numShards(), 3u);
}

// num-shards: 0 must throw.
TEST_F(HDF5BsasGen1ReaderTest, ConfigNumShardsZeroThrows)
{
    auto cfg = makeConfigFromYaml(
        "name: test_reader\n" "file-path: " + mockFile_ + "\n" "num-shards: 0\n");

    EXPECT_THROW(HDF5BsasGen1ReaderConfig{cfg}, HDF5BsasGen1ReaderConfig::Error);
}

// With num-shards: 3, slots must fall in the 3-shard ranges.
TEST_F(HDF5BsasGen1ReaderTest, ShardSlotRespectCustomNumShards)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_shard_3\n" "file-path: " + mockFile_ + "\n" "chunk-size: 1000\n" "num-shards: 3\n");

    {
        HDF5BsasGen1Reader reader(bus, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    auto batches = bus->snapshot();
    ASSERT_GE(batches.size(), 1u);
    const auto slots = collectShardSlots(batches[0]);
    ASSERT_EQ(slots.size(), kFloatCols + kIntCols);

    constexpr int numShards = 3;
    constexpr int shardSize = 65536 / numShards; // 21845
    std::array<bool, numShards> covered{};
    covered.fill(false);

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        const int v = std::stoi(slots[i]);
        const int expectedShard = static_cast<int>(i % static_cast<std::size_t>(numShards));
        const int lo = expectedShard * shardSize;
        const int hi = (expectedShard == numShards - 1) ? 65535 : (lo + shardSize - 1);
        EXPECT_GE(v, lo) << "col " << i << " slot " << v << " below shard " << expectedShard;
        EXPECT_LE(v, hi) << "col " << i << " slot " << v << " above shard " << expectedShard;
        covered[static_cast<std::size_t>(expectedShard)] = true;
    }

    for (int i = 0; i < numShards; ++i)
        EXPECT_TRUE(covered[static_cast<std::size_t>(i)]) << "shard " << i << " has no column";
}

#endif // MLDP_PVXS_HDF5_ENABLED
