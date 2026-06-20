//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "BsasGen1HDF5Mock.h"

#include <H5Cpp.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace mldp_pvxs_driver::test::mock {

namespace {

constexpr std::size_t kStringLen = 36;

void writeStringAttr(H5::H5Object& obj, const char* name, const char* value)
{
    H5::StrType strType(H5::PredType::C_S1, std::strlen(value) + 1);
    strType.setStrpad(H5T_STR_NULLTERM);
    strType.setCset(H5T_CSET_UTF8);
    H5::DataSpace scalar(H5S_SCALAR);
    auto attr = obj.createAttribute(name, strType, scalar);
    attr.write(strType, value);
}

void writeInt64Attr(H5::H5Object& obj, const char* name, int64_t value)
{
    H5::DataSpace scalar(H5S_SCALAR);
    auto attr = obj.createAttribute(name, H5::PredType::STD_I64LE, scalar);
    attr.write(H5::PredType::NATIVE_INT64, &value);
}

void writeBoolAttr(H5::H5Object& obj, const char* name, bool value)
{
    H5::DataSpace scalar(H5S_SCALAR);
    H5::IntType uint8Type(H5::PredType::NATIVE_UINT8);
    auto attr = obj.createAttribute(name, uint8Type, scalar);
    uint8_t v = value ? 1 : 0;
    attr.write(uint8Type, &v);
}

void writeNullTitleAttr(H5::H5Object& obj)
{
    H5::StrType strType(H5::PredType::C_S1, 1);
    strType.setStrpad(H5T_STR_NULLTERM);
    strType.setCset(H5T_CSET_UTF8);
    H5::DataSpace nullSpace(H5S_NULL);
    obj.createAttribute("TITLE", strType, nullSpace);
}

H5::DataSet writeFixedStringDataset(H5::Group& group, const char* name,
                                     const std::vector<std::string>& strings,
                                     std::size_t fixedLen,
                                     H5T_str_t strpad = H5T_STR_NULLTERM)
{
    H5::StrType strType(H5::PredType::C_S1, fixedLen);
    strType.setStrpad(strpad);
    strType.setCset(H5T_CSET_ASCII);

    hsize_t dims[1] = {strings.size()};
    H5::DataSpace space(1, dims);
    auto ds = group.createDataSet(name, strType, space);

    std::vector<char> buf(strings.size() * fixedLen, '\0');
    for (std::size_t i = 0; i < strings.size(); ++i)
    {
        const auto& s = strings[i];
        std::size_t maxCopy = (strpad == H5T_STR_NULLTERM) ? fixedLen - 1 : fixedLen;
        std::size_t copyLen = std::min(s.size(), maxCopy);
        std::memcpy(buf.data() + i * fixedLen, s.c_str(), copyLen);
    }
    ds.write(buf.data(), strType);

    writeStringAttr(ds, "CLASS", "ARRAY");
    writeStringAttr(ds, "FLAVOR", "numpy");
    writeNullTitleAttr(ds);
    writeStringAttr(ds, "VERSION", "1.0");
    writeStringAttr(ds, "kind", "string");
    writeStringAttr(ds, "name", "N.");
    writeBoolAttr(ds, "transposed", false);

    return ds;
}

void addItemsDatasetVarietyAttr(H5::DataSet& ds)
{
    writeStringAttr(ds, "kind", "string");
    writeStringAttr(ds, "name", "N.");
    writeBoolAttr(ds, "transposed", false);
}

std::string makeFloatColName(std::size_t idx)
{
    std::ostringstream oss;
    oss << "SIG_" << std::setfill('0') << std::setw(4) << idx;
    return oss.str();
}

std::string makeIntColName(std::size_t idx)
{
    std::ostringstream oss;
    oss << "FLAG_" << std::setfill('0') << std::setw(2) << idx;
    return oss.str();
}

} // anonymous namespace

void BsasGen1HDF5Mock::generate(const std::string& outputPath, const Params& params)
{
    H5::H5File file(outputPath, H5F_ACC_TRUNC);

    // Root attributes
    writeStringAttr(file, "CLASS", "GROUP");
    writeStringAttr(file, "PYTABLES_FORMAT_VERSION", "2.0");
    writeNullTitleAttr(file);
    writeStringAttr(file, "VERSION", "1.0");

    // /data group
    auto dataGroup = file.createGroup("data");
    writeStringAttr(dataGroup, "CLASS", "GROUP");
    writeNullTitleAttr(dataGroup);
    writeStringAttr(dataGroup, "VERSION", "1.0");
    writeStringAttr(dataGroup, "axis0_variety", "regular");
    writeStringAttr(dataGroup, "axis1_variety", "regular");
    writeStringAttr(dataGroup, "block0_items_variety", "regular");
    writeStringAttr(dataGroup, "block1_items_variety", "regular");
    writeStringAttr(dataGroup, "block2_items_variety", "regular");
    writeStringAttr(dataGroup, "encoding", "UTF-8");
    writeStringAttr(dataGroup, "errors", "strict");
    writeInt64Attr(dataGroup, "nblocks", 3);
    writeInt64Attr(dataGroup, "ndim", 2);
    writeStringAttr(dataGroup, "pandas_type", "frame");
    writeStringAttr(dataGroup, "pandas_version", "0.15.2");

    // Build column name lists
    std::vector<std::string> floatCols(params.numFloatCols);
    for (std::size_t i = 0; i < params.numFloatCols; ++i)
        floatCols[i] = makeFloatColName(i);

    std::vector<std::string> intCols(params.numIntCols);
    for (std::size_t i = 0; i < params.numIntCols; ++i)
        intCols[i] = makeIntColName(i);

    std::vector<std::string> tsCols = {"secondsPastEpoch", "nanoseconds"};

    // axis0: all column names
    std::vector<std::string> allCols;
    allCols.insert(allCols.end(), floatCols.begin(), floatCols.end());
    allCols.insert(allCols.end(), intCols.begin(), intCols.end());
    allCols.insert(allCols.end(), tsCols.begin(), tsCols.end());

    writeFixedStringDataset(dataGroup, "axis0", allCols, kStringLen);

    // axis1: row indices as int64
    {
        hsize_t dims[1] = {params.numRows};
        H5::DataSpace space(1, dims);
        auto ds = dataGroup.createDataSet("axis1", H5::PredType::STD_I64LE, space);
        std::vector<int64_t> indices(params.numRows);
        for (std::size_t i = 0; i < params.numRows; ++i)
            indices[i] = static_cast<int64_t>(i);
        ds.write(indices.data(), H5::PredType::NATIVE_INT64);

        writeStringAttr(ds, "CLASS", "ARRAY");
        writeStringAttr(ds, "FLAVOR", "numpy");
        writeNullTitleAttr(ds);
        writeStringAttr(ds, "VERSION", "1.0");
        writeStringAttr(ds, "kind", "integer");
        writeStringAttr(ds, "name", "N.");
        writeBoolAttr(ds, "transposed", false);
    }

    // block0: float64 columns
    writeFixedStringDataset(dataGroup, "block0_items", floatCols, kStringLen);

    {
        hsize_t dims[2] = {params.numRows, params.numFloatCols};
        H5::DataSpace space(2, dims);
        auto ds = dataGroup.createDataSet("block0_values", H5::PredType::IEEE_F64LE, space);

        std::vector<double> data(params.numRows * params.numFloatCols);
        for (std::size_t r = 0; r < params.numRows; ++r)
            for (std::size_t c = 0; c < params.numFloatCols; ++c)
                data[r * params.numFloatCols + c] =
                    std::sin(static_cast<double>(r) * 0.1 + static_cast<double>(c) * 0.01);
        ds.write(data.data(), H5::PredType::NATIVE_DOUBLE);

        writeStringAttr(ds, "CLASS", "ARRAY");
        writeStringAttr(ds, "FLAVOR", "numpy");
        writeNullTitleAttr(ds);
        writeStringAttr(ds, "VERSION", "1.0");
        writeBoolAttr(ds, "transposed", true);
    }

    // block1: int16 columns
    {
        std::size_t strLen = 26;
        writeFixedStringDataset(dataGroup, "block1_items", intCols, strLen);
    }

    {
        hsize_t dims[2] = {params.numRows, params.numIntCols};
        H5::DataSpace space(2, dims);
        auto ds = dataGroup.createDataSet("block1_values", H5::PredType::STD_I16LE, space);

        std::vector<int16_t> data(params.numRows * params.numIntCols);
        for (std::size_t r = 0; r < params.numRows; ++r)
            for (std::size_t c = 0; c < params.numIntCols; ++c)
                data[r * params.numIntCols + c] =
                    static_cast<int16_t>(r + c);
        ds.write(data.data(), H5::PredType::NATIVE_INT16);

        writeStringAttr(ds, "CLASS", "ARRAY");
        writeStringAttr(ds, "FLAVOR", "numpy");
        writeNullTitleAttr(ds);
        writeStringAttr(ds, "VERSION", "1.0");
        writeBoolAttr(ds, "transposed", true);
    }

    // block2: uint32 timestamp columns
    {
        writeFixedStringDataset(dataGroup, "block2_items", tsCols, 16, H5T_STR_NULLPAD);
    }

    {
        hsize_t dims[2] = {params.numRows, 2};
        H5::DataSpace space(2, dims);
        auto ds = dataGroup.createDataSet("block2_values", H5::PredType::STD_U32LE, space);

        std::vector<uint32_t> data(params.numRows * 2);
        for (std::size_t r = 0; r < params.numRows; ++r)
        {
            data[r * 2 + 0] = params.baseEpoch + static_cast<uint32_t>(r);
            data[r * 2 + 1] = static_cast<uint32_t>(r * 1000);
        }
        ds.write(data.data(), H5::PredType::NATIVE_UINT32);

        writeStringAttr(ds, "CLASS", "ARRAY");
        writeStringAttr(ds, "FLAVOR", "numpy");
        writeNullTitleAttr(ds);
        writeStringAttr(ds, "VERSION", "1.0");
        writeBoolAttr(ds, "transposed", true);
    }
}

} // namespace mldp_pvxs_driver::test::mock
