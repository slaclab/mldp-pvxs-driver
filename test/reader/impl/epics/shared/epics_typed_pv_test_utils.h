#pragma once

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <util/bus/IDataBus.h>

namespace epics_typed_pv_test_utils {

using DataBatch      = mldp_pvxs_driver::util::bus::DataBatch;
using DataColumn     = mldp_pvxs_driver::util::bus::DataColumn;
using ColumnValues   = mldp_pvxs_driver::util::bus::ColumnValues;

inline const std::vector<std::string>& allTypedCoveragePvs()
{
    static const std::vector<std::string> kPvs{
        "test:counter",
        "test:voltage",
        "test:status",
        "test:waveform",
        "test:bool",
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
        "test:string",
        "test:bool_array",
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
        "test:string_array",
    };
    return kPvs;
}

inline std::set<std::string> allTypedCoveragePvsSet()
{
    const auto& pvs = allTypedCoveragePvs();
    return std::set<std::string>(pvs.begin(), pvs.end());
}

inline std::string buildTypedCoverageYaml(const std::string& readerName)
{
    std::string yaml = "name: " + readerName + "\npvs:\n";
    for (const auto& pv : allTypedCoveragePvs())
    {
        yaml += "  - name: " + pv + "\n";
    }
    return yaml;
}

/// Return a pointer to the first DataColumn with the given name, or nullptr.
inline const DataColumn* findDataColumn(const DataBatch& batch, const std::string& name)
{
    for (const auto& col : batch.columns)
    {
        if (col.name == name)
            return &col;
    }
    return nullptr;
}

/// Return true if any column (regular or enum) in the batch has the given name.
inline bool hasColumnWithName(const DataBatch& batch, const std::string& name)
{
    for (const auto& col : batch.columns)
        if (col.name == name)
            return true;
    for (const auto& col : batch.enum_columns)
        if (col.name == name)
            return true;
    return false;
}

template <typename FinderFn>
void assertTypedCoverageDataFrames(const FinderFn& findLatestDataFrameForSourceFn)
{
    const auto requireLatest = [&](const std::string& pv) -> const DataBatch*
    {
        const auto* batch = findLatestDataFrameForSourceFn(pv);
        EXPECT_NE(batch, nullptr) << "Missing dataframe for " << pv;
        return batch;
    };

    const auto expectInt32Scalar = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing int32 column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<int32_t>>(col->values))
            << "Column " << pv << " is not int32";
        EXPECT_GT(std::get<std::vector<int32_t>>(col->values).size(), 0u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectInt64Scalar = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing int64 column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<int64_t>>(col->values))
            << "Column " << pv << " is not int64";
        EXPECT_GT(std::get<std::vector<int64_t>>(col->values).size(), 0u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectFloatScalar = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing float column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<float>>(col->values))
            << "Column " << pv << " is not float";
        EXPECT_GT(std::get<std::vector<float>>(col->values).size(), 0u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectDoubleScalar = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing double column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<double>>(col->values))
            << "Column " << pv << " is not double";
        EXPECT_GT(std::get<std::vector<double>>(col->values).size(), 0u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectBoolScalar = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing bool column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<bool>>(col->values))
            << "Column " << pv << " is not bool";
        EXPECT_GT(std::get<std::vector<bool>>(col->values).size(), 0u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectStringScalar = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing string column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::string>>(col->values))
            << "Column " << pv << " is not string";
        EXPECT_GT(std::get<std::vector<std::string>>(col->values).size(), 0u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectBoolArray = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing bool array column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::vector<bool>>>(col->values))
            << "Column " << pv << " is not bool array";
        const auto& rows = std::get<std::vector<std::vector<bool>>>(col->values);
        EXPECT_GT(rows.size(), 0u);
        EXPECT_EQ(rows[0].size(), 4u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectInt32Array = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing int32 array column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::vector<int32_t>>>(col->values))
            << "Column " << pv << " is not int32 array";
        const auto& rows = std::get<std::vector<std::vector<int32_t>>>(col->values);
        EXPECT_GT(rows.size(), 0u);
        EXPECT_EQ(rows[0].size(), 4u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectInt64Array = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing int64 array column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::vector<int64_t>>>(col->values))
            << "Column " << pv << " is not int64 array";
        const auto& rows = std::get<std::vector<std::vector<int64_t>>>(col->values);
        EXPECT_GT(rows.size(), 0u);
        EXPECT_EQ(rows[0].size(), 4u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectFloatArray = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing float array column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::vector<float>>>(col->values))
            << "Column " << pv << " is not float array";
        const auto& rows = std::get<std::vector<std::vector<float>>>(col->values);
        EXPECT_GT(rows.size(), 0u);
        EXPECT_EQ(rows[0].size(), 4u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectDoubleArray = [&](const std::string& pv, int expectedSize)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing double array column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::vector<double>>>(col->values))
            << "Column " << pv << " is not double array";
        const auto& rows = std::get<std::vector<std::vector<double>>>(col->values);
        EXPECT_GT(rows.size(), 0u);
        EXPECT_EQ(static_cast<int>(rows[0].size()), expectedSize);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };
    const auto expectStringArrayAsDataColumn = [&](const std::string& pv)
    {
        const auto* batch = requireLatest(pv);
        if (!batch)
            return;
        const auto* col = findDataColumn(*batch, pv);
        EXPECT_NE(col, nullptr) << "Missing string array column for " << pv;
        if (!col)
            return;
        EXPECT_TRUE(std::holds_alternative<std::vector<std::string>>(col->values))
            << "Column " << pv << " is not string (array)";
        const auto& vals = std::get<std::vector<std::string>>(col->values);
        EXPECT_EQ(vals.size(), 4u);
        EXPECT_FALSE(hasColumnWithName(*batch, "value.timeStamp"));
    };

    expectInt32Scalar("test:counter");
    expectInt32Scalar("test:int8");
    expectInt32Scalar("test:int16");
    expectInt32Scalar("test:int32");
    expectInt32Scalar("test:uint8");
    expectInt32Scalar("test:uint16");
    expectInt32Scalar("test:uint32");
    expectInt64Scalar("test:int64");
    expectInt64Scalar("test:uint64");
    expectFloatScalar("test:float32");
    expectDoubleScalar("test:voltage");
    expectDoubleScalar("test:float64");
    expectBoolScalar("test:bool");
    expectStringScalar("test:status");
    expectStringScalar("test:string");

    expectDoubleArray("test:waveform", 256);
    expectBoolArray("test:bool_array");
    expectInt32Array("test:int8_array");
    expectInt32Array("test:int16_array");
    expectInt32Array("test:int32_array");
    expectInt32Array("test:uint8_array");
    expectInt32Array("test:uint16_array");
    expectInt32Array("test:uint32_array");
    expectInt64Array("test:int64_array");
    expectInt64Array("test:uint64_array");
    expectFloatArray("test:float32_array");
    expectDoubleArray("test:float64_array", 4);
    expectStringArrayAsDataColumn("test:string_array");
}

} // namespace epics_typed_pv_test_utils
