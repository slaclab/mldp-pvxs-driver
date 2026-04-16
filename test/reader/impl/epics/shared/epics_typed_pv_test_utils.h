#pragma once

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <util/bus/IDataBus.h>

namespace epics_typed_pv_test_utils {

using DataFrame = dp::service::common::DataFrame;

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

inline bool hasColumnWithName(const DataFrame& df, const std::string& name)
{
    for (int i = 0; i < df.stringcolumns_size(); ++i)
        if (df.stringcolumns(i).name() == name)
            return true;
    for (int i = 0; i < df.int32columns_size(); ++i)
        if (df.int32columns(i).name() == name)
            return true;
    for (int i = 0; i < df.int64columns_size(); ++i)
        if (df.int64columns(i).name() == name)
            return true;
    for (int i = 0; i < df.floatcolumns_size(); ++i)
        if (df.floatcolumns(i).name() == name)
            return true;
    for (int i = 0; i < df.doublecolumns_size(); ++i)
        if (df.doublecolumns(i).name() == name)
            return true;
    for (int i = 0; i < df.boolcolumns_size(); ++i)
        if (df.boolcolumns(i).name() == name)
            return true;
    return false;
}

template <typename FinderFn>
void assertTypedCoverageDataFrames(const FinderFn& findLatestDataFrameForSourceFn)
{
    const auto requireLatest = [&](const std::string& pv) -> const DataFrame*
    {
        const auto* df = findLatestDataFrameForSourceFn(pv);
        EXPECT_NE(df, nullptr) << "Missing dataframe for " << pv;
        return df;
    };

    const auto expectInt32Scalar = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->int32columns_size(), 0);
        EXPECT_EQ(df->int32columns(0).name(), pv);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectInt64Scalar = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->int64columns_size(), 0);
        EXPECT_EQ(df->int64columns(0).name(), pv);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectFloatScalar = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->floatcolumns_size(), 0);
        EXPECT_EQ(df->floatcolumns(0).name(), pv);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectDoubleScalar = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->doublecolumns_size(), 0);
        EXPECT_EQ(df->doublecolumns(0).name(), pv);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectBoolScalar = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->boolcolumns_size(), 0);
        EXPECT_EQ(df->boolcolumns(0).name(), pv);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectStringScalar = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->stringcolumns_size(), 0);
        EXPECT_EQ(df->stringcolumns(0).name(), pv);
        EXPECT_GT(df->stringcolumns(0).values_size(), 0);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectBoolArray = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->boolarraycolumns_size(), 0);
        EXPECT_EQ(df->boolarraycolumns(0).name(), pv);
        EXPECT_EQ(df->boolarraycolumns(0).values_size(), 4);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectInt32Array = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->int32arraycolumns_size(), 0);
        EXPECT_EQ(df->int32arraycolumns(0).name(), pv);
        EXPECT_EQ(df->int32arraycolumns(0).values_size(), 4);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectInt64Array = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->int64arraycolumns_size(), 0);
        EXPECT_EQ(df->int64arraycolumns(0).name(), pv);
        EXPECT_EQ(df->int64arraycolumns(0).values_size(), 4);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectFloatArray = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->floatarraycolumns_size(), 0);
        EXPECT_EQ(df->floatarraycolumns(0).name(), pv);
        EXPECT_EQ(df->floatarraycolumns(0).values_size(), 4);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectDoubleArray = [&](const std::string& pv, int expectedSize)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->doublearraycolumns_size(), 0);
        EXPECT_EQ(df->doublearraycolumns(0).name(), pv);
        EXPECT_EQ(df->doublearraycolumns(0).values_size(), expectedSize);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
    };
    const auto expectStringArrayAsDataColumn = [&](const std::string& pv)
    {
        const auto* df = requireLatest(pv);
        if (!df)
            return;
        EXPECT_GT(df->datacolumns_size(), 0);
        EXPECT_EQ(df->datacolumns(0).name(), pv);
        EXPECT_GT(df->datacolumns(0).datavalues_size(), 0);
        EXPECT_EQ(df->datacolumns(0).datavalues(0).arrayvalue().datavalues_size(), 4);
        EXPECT_FALSE(hasColumnWithName(*df, "value.timeStamp"));
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
