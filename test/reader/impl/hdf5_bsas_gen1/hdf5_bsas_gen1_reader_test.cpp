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
    static constexpr std::size_t kIntCols   = 4;
    static constexpr std::size_t kRows      = 20;
    static constexpr uint32_t    kBaseEpoch = 1700000000u;

    void SetUp() override
    {
        tempDir_ = fs::path(MLDP_TEST_DATA_DIR) / "hdf5";
        fs::create_directories(tempDir_);
        mockFile_ = (tempDir_ / "mock_bsas_gen1.h5").string();

        BsasGen1HDF5Mock::Params params;
        params.numFloatCols = kFloatCols;
        params.numIntCols   = kIntCols;
        params.numRows      = kRows;
        params.baseEpoch    = kBaseEpoch;
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
        "name: test_reader\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 5\n");

    HDF5BsasGen1ReaderConfig config(cfg);
    EXPECT_TRUE(config.valid());
    EXPECT_EQ(config.name(), "test_reader");
    EXPECT_EQ(config.filePath(), mockFile_);
    EXPECT_EQ(config.chunkSize(), 5u);
    EXPECT_EQ(config.groupName(), "data");
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
        "name: bsas_test\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 1000\n");

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

TEST_F(HDF5BsasGen1ReaderTest, ChunkedReadingProducesMultipleBatches)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_chunked\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 7\n");

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
        "name: bsas_ts\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 1000\n");

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
        "name: bsas_float\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 1000\n");

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
        "name: bsas_int\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 1000\n");

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

TEST_F(HDF5BsasGen1ReaderTest, ColumnNamesMatchBlockItems)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_names\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 1000\n");

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

TEST_F(HDF5BsasGen1ReaderTest, ReaderNameMatches)
{
    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: my_reader\n"
        "file-path: " + mockFile_ + "\n"
        "chunk-size: 1000\n");

    HDF5BsasGen1Reader reader(bus, nullptr, cfg);
    EXPECT_EQ(reader.name(), "my_reader");
}

// ---------------------------------------------------------------------------
// Mock structure validation — verify generated file matches reference format
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, MockFileMatchesReferenceStructure)
{
    // Open the mock-generated file and verify its HDF5 structure matches
    // the real bsas-gen1-extract.h5 file format exactly.
    H5::H5File file(mockFile_, H5F_ACC_RDONLY);

    // Root attributes
    ASSERT_TRUE(file.attrExists("CLASS"));
    ASSERT_TRUE(file.attrExists("PYTABLES_FORMAT_VERSION"));
    ASSERT_TRUE(file.attrExists("VERSION"));
    ASSERT_TRUE(file.attrExists("TITLE"));

    // /data group exists with correct attributes
    ASSERT_TRUE(file.nameExists("data"));
    H5::Group dataGroup = file.openGroup("data");
    ASSERT_TRUE(dataGroup.attrExists("CLASS"));
    ASSERT_TRUE(dataGroup.attrExists("VERSION"));
    ASSERT_TRUE(dataGroup.attrExists("TITLE"));
    ASSERT_TRUE(dataGroup.attrExists("axis0_variety"));
    ASSERT_TRUE(dataGroup.attrExists("axis1_variety"));
    ASSERT_TRUE(dataGroup.attrExists("block0_items_variety"));
    ASSERT_TRUE(dataGroup.attrExists("block1_items_variety"));
    ASSERT_TRUE(dataGroup.attrExists("block2_items_variety"));
    ASSERT_TRUE(dataGroup.attrExists("nblocks"));
    ASSERT_TRUE(dataGroup.attrExists("ndim"));
    ASSERT_TRUE(dataGroup.attrExists("pandas_type"));
    ASSERT_TRUE(dataGroup.attrExists("pandas_version"));
    ASSERT_TRUE(dataGroup.attrExists("encoding"));
    ASSERT_TRUE(dataGroup.attrExists("errors"));

    // Verify nblocks=3, ndim=2
    {
        int64_t nblocks = 0;
        dataGroup.openAttribute("nblocks").read(H5::PredType::NATIVE_INT64, &nblocks);
        EXPECT_EQ(nblocks, 3);
        int64_t ndim = 0;
        dataGroup.openAttribute("ndim").read(H5::PredType::NATIVE_INT64, &ndim);
        EXPECT_EQ(ndim, 2);
    }

    // axis0: 1D string dataset with all column names
    ASSERT_TRUE(dataGroup.nameExists("axis0"));
    {
        H5::DataSet ds = dataGroup.openDataSet("axis0");
        H5::DataSpace sp = ds.getSpace();
        ASSERT_EQ(sp.getSimpleExtentNdims(), 1);
        hsize_t dims[1]{0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kFloatCols + kIntCols + 2); // +2 for timestamp cols
        // Must be fixed-length string
        H5::DataType dt = ds.getDataType();
        EXPECT_EQ(dt.getClass(), H5T_STRING);
        ASSERT_TRUE(ds.attrExists("CLASS"));
        ASSERT_TRUE(ds.attrExists("FLAVOR"));
    }

    // axis1: 1D int64 dataset with row indices
    ASSERT_TRUE(dataGroup.nameExists("axis1"));
    {
        H5::DataSet ds = dataGroup.openDataSet("axis1");
        H5::DataSpace sp = ds.getSpace();
        hsize_t dims[1]{0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_INTEGER);
    }

    // block0_items: 1D fixed string[numFloatCols]
    ASSERT_TRUE(dataGroup.nameExists("block0_items"));
    {
        H5::DataSet ds = dataGroup.openDataSet("block0_items");
        hsize_t dims[1]{0};
        ds.getSpace().getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kFloatCols);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_STRING);
    }

    // block0_values: 2D float64 (rows x floatCols)
    ASSERT_TRUE(dataGroup.nameExists("block0_values"));
    {
        H5::DataSet ds = dataGroup.openDataSet("block0_values");
        H5::DataSpace sp = ds.getSpace();
        ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
        hsize_t dims[2]{0, 0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(dims[1], kFloatCols);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_FLOAT);
        EXPECT_EQ(ds.getDataType().getSize(), 8u); // float64
        ASSERT_TRUE(ds.attrExists("transposed"));
    }

    // block1_items: 1D fixed string[numIntCols]
    ASSERT_TRUE(dataGroup.nameExists("block1_items"));
    {
        H5::DataSet ds = dataGroup.openDataSet("block1_items");
        hsize_t dims[1]{0};
        ds.getSpace().getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kIntCols);
    }

    // block1_values: 2D int16 (rows x intCols)
    ASSERT_TRUE(dataGroup.nameExists("block1_values"));
    {
        H5::DataSet ds = dataGroup.openDataSet("block1_values");
        H5::DataSpace sp = ds.getSpace();
        ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
        hsize_t dims[2]{0, 0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(dims[1], kIntCols);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_INTEGER);
        EXPECT_EQ(ds.getDataType().getSize(), 2u); // int16
    }

    // block2_items: 1D fixed string[2] = {"secondsPastEpoch", "nanoseconds"}
    ASSERT_TRUE(dataGroup.nameExists("block2_items"));
    {
        H5::DataSet ds = dataGroup.openDataSet("block2_items");
        hsize_t dims[1]{0};
        ds.getSpace().getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], 2u);

        H5::StrType strType = ds.getStrType();
        std::size_t strLen = strType.getSize();
        std::vector<char> buf(2 * strLen, '\0');
        ds.read(buf.data(), strType);
        std::string col0(buf.data(), strnlen(buf.data(), strLen));
        std::string col1(buf.data() + strLen, strnlen(buf.data() + strLen, strLen));
        EXPECT_EQ(col0, "secondsPastEpoch");
        EXPECT_EQ(col1, "nanoseconds");
    }

    // block2_values: 2D uint32 (rows x 2)
    ASSERT_TRUE(dataGroup.nameExists("block2_values"));
    {
        H5::DataSet ds = dataGroup.openDataSet("block2_values");
        H5::DataSpace sp = ds.getSpace();
        ASSERT_EQ(sp.getSimpleExtentNdims(), 2);
        hsize_t dims[2]{0, 0};
        sp.getSimpleExtentDims(dims);
        EXPECT_EQ(dims[0], kRows);
        EXPECT_EQ(dims[1], 2u);
        EXPECT_EQ(ds.getDataType().getClass(), H5T_INTEGER);
        EXPECT_EQ(ds.getDataType().getSize(), 4u); // uint32
    }
}

// ---------------------------------------------------------------------------
// Compare mock to real reference file (data/bsas-gen1-extract.h5)
// Verify identical structure: same datasets, same dtypes, same attrs.
// ---------------------------------------------------------------------------

TEST_F(HDF5BsasGen1ReaderTest, MockMatchesRealReferenceFileStructure)
{
    if (!std::getenv("BSAS_GEN1_REFERENCE_TEST"))
    {
        GTEST_SKIP() << "Set BSAS_GEN1_REFERENCE_TEST=1 to enable (requires data/bsas-gen1-extract.h5)";
    }

    const std::string refPath = std::string(MLDP_TEST_DATA_DIR) + "/bsas-gen1-extract.h5";
    if (!fs::exists(refPath))
    {
        GTEST_SKIP() << "Reference file not found: " << refPath;
    }

    // Generate mock with same dimensions as reference: 1351 float, 16 int, 19 rows
    const std::string matchFile = (tempDir_ / "mock_ref_match.h5").string();
    BsasGen1HDF5Mock::Params refParams;
    refParams.numFloatCols = 1351;
    refParams.numIntCols   = 16;
    refParams.numRows      = 19;
    refParams.baseEpoch    = 1700000000u;
    BsasGen1HDF5Mock::generate(matchFile, refParams);

    H5::H5File refFile(refPath, H5F_ACC_RDONLY);
    H5::H5File mockFile(matchFile, H5F_ACC_RDONLY);

    H5::Group refData = refFile.openGroup("data");
    H5::Group mockData = mockFile.openGroup("data");

    // Verify datasets exist with same shapes and dtypes
    auto checkDataset = [&](const char* name) {
        ASSERT_TRUE(refData.nameExists(name)) << name << " missing in reference";
        ASSERT_TRUE(mockData.nameExists(name)) << name << " missing in mock";

        H5::DataSet refDs = refData.openDataSet(name);
        H5::DataSet mockDs = mockData.openDataSet(name);

        // Same rank
        int refNdims = refDs.getSpace().getSimpleExtentNdims();
        int mockNdims = mockDs.getSpace().getSimpleExtentNdims();
        EXPECT_EQ(refNdims, mockNdims) << name << " rank mismatch";

        // Same shape
        std::vector<hsize_t> refDims(refNdims), mockDims(mockNdims);
        refDs.getSpace().getSimpleExtentDims(refDims.data());
        mockDs.getSpace().getSimpleExtentDims(mockDims.data());
        for (int i = 0; i < refNdims; ++i)
            EXPECT_EQ(refDims[i], mockDims[i]) << name << " dim[" << i << "] mismatch";

        // Same dtype class
        EXPECT_EQ(refDs.getDataType().getClass(), mockDs.getDataType().getClass())
            << name << " dtype class mismatch";

        // Same dtype size
        EXPECT_EQ(refDs.getDataType().getSize(), mockDs.getDataType().getSize())
            << name << " dtype size mismatch";
    };

    checkDataset("axis0");
    checkDataset("axis1");
    checkDataset("block0_items");
    checkDataset("block0_values");
    checkDataset("block1_items");
    checkDataset("block1_values");
    checkDataset("block2_items");
    checkDataset("block2_values");

    fs::remove(matchFile);
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
    const char* envInt   = std::getenv("BSAS_GEN1_TEST_INT_COLS");
    const char* envRows  = std::getenv("BSAS_GEN1_TEST_ROWS");
    const char* envChunk = std::getenv("BSAS_GEN1_TEST_CHUNK_SIZE");

    const std::size_t numFloatCols = envFloat ? static_cast<std::size_t>(std::atol(envFloat)) : 100;
    const std::size_t numIntCols   = envInt   ? static_cast<std::size_t>(std::atol(envInt))   : 16;
    const std::size_t numRows      = envRows  ? static_cast<std::size_t>(std::atol(envRows))  : 5000;
    const std::size_t chunkSize    = envChunk ? static_cast<std::size_t>(std::atol(envChunk)) : 512;
    constexpr uint32_t baseEpoch   = 1700000000u;

    const std::string largeFile = (tempDir_ / "mock_bsas_large.h5").string();

    BsasGen1HDF5Mock::Params params;
    params.numFloatCols = numFloatCols;
    params.numIntCols   = numIntCols;
    params.numRows      = numRows;
    params.baseEpoch    = baseEpoch;
    BsasGen1HDF5Mock::generate(largeFile, params);

    auto bus = std::make_shared<MockDataBus>();
    auto cfg = makeConfigFromYaml(
        "name: bsas_large\n"
        "file-path: " + largeFile + "\n"
        "chunk-size: " + std::to_string(chunkSize) + "\n");

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
                int32_t expected = static_cast<int32_t>(globalRow + c);
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

#endif // MLDP_PVXS_HDF5_ENABLED
