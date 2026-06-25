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

void writeByteStringAttr(H5::H5Object& obj, const char* name, const char* value)
{
    H5::StrType strType(H5::PredType::C_S1, std::strlen(value));
    strType.setStrpad(H5T_STR_NULLTERM);
    strType.setCset(H5T_CSET_ASCII);
    H5::DataSpace scalar(H5S_SCALAR);
    auto attr = obj.createAttribute(name, strType, scalar);
    attr.write(strType, value);
}

std::string makeFloatColName(const std::string& prefix, std::size_t idx)
{
    std::ostringstream oss;
    oss << prefix << std::setfill('0') << std::setw(4) << idx;
    return oss.str();
}

std::string makeIntColName(const std::string& prefix, std::size_t idx)
{
    std::ostringstream oss;
    oss << prefix << std::setfill('0') << std::setw(2) << idx;
    return oss.str();
}

std::string makeLabel(const std::string& name)
{
    std::string label = name;
    for (auto& c : label)
        if (c == '_') c = ':';
    return label;
}

} // anonymous namespace

void BsasGen1HDF5Mock::generate(const std::string& outputPath, const Params& params)
{
    H5::H5File file(outputPath, H5F_ACC_TRUNC);

    const hsize_t dims[2] = {params.numRows, 1};
    H5::DataSpace space(2, dims);

    // Float64 datasets
    for (std::size_t c = 0; c < params.numFloatCols; ++c)
    {
        std::string name = makeFloatColName(params.floatColPrefix, c);
        auto ds = file.createDataSet(name, H5::PredType::IEEE_F64LE, space);

        std::vector<double> data(params.numRows);
        for (std::size_t r = 0; r < params.numRows; ++r)
            data[r] = std::sin(static_cast<double>(r) * 0.1 + static_cast<double>(c) * 0.01);
        ds.write(data.data(), H5::PredType::NATIVE_DOUBLE);

        writeByteStringAttr(ds, "MATLAB_class", "double");
        writeByteStringAttr(ds, "label", makeLabel(name).c_str());
    }

    // Int16 datasets
    for (std::size_t c = 0; c < params.numIntCols; ++c)
    {
        std::string name = makeIntColName(params.intColPrefix, c);
        auto ds = file.createDataSet(name, H5::PredType::STD_I16LE, space);

        std::vector<int16_t> data(params.numRows);
        for (std::size_t r = 0; r < params.numRows; ++r)
            data[r] = static_cast<int16_t>(r + c);
        ds.write(data.data(), H5::PredType::NATIVE_INT16);

        writeByteStringAttr(ds, "MATLAB_class", "int16");
        writeByteStringAttr(ds, "label", makeLabel(name).c_str());
    }

    // Timestamp datasets
    {
        auto ds = file.createDataSet("secondsPastEpoch", H5::PredType::STD_U32LE, space);
        std::vector<uint32_t> data(params.numRows);
        for (std::size_t r = 0; r < params.numRows; ++r)
            data[r] = params.baseEpoch + static_cast<uint32_t>(r);
        ds.write(data.data(), H5::PredType::NATIVE_UINT32);

        writeByteStringAttr(ds, "MATLAB_class", "uint32");
        writeByteStringAttr(ds, "label", "secondsPastEpoch");
    }

    {
        auto ds = file.createDataSet("nanoseconds", H5::PredType::STD_U32LE, space);
        std::vector<uint32_t> data(params.numRows);
        for (std::size_t r = 0; r < params.numRows; ++r)
            data[r] = static_cast<uint32_t>(r * 1000);
        ds.write(data.data(), H5::PredType::NATIVE_UINT32);

        writeByteStringAttr(ds, "MATLAB_class", "uint32");
        writeByteStringAttr(ds, "label", "nanoseconds");
    }
}

} // namespace mldp_pvxs_driver::test::mock
