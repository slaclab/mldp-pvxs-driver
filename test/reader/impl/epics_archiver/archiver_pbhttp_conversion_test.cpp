//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_archiver/ArchiverPbHttpConversion.h>

#include <EPICSEvent.pb.h>
#include <util/bus/DataBatch.h>
#include <util/time/DateTimeUtils.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

using namespace mldp_pvxs_driver::reader::impl::epics_archiver;
using namespace mldp_pvxs_driver::util::time;
using namespace mldp_pvxs_driver::util::bus;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a minimal PayloadInfo header for the given type and year.
EPICS::PayloadInfo makeHeader(EPICS::PayloadType type, int year = 2024, const std::string& pvname = "TEST:PV")
{
    EPICS::PayloadInfo hdr;
    hdr.set_type(type);
    hdr.set_year(year);
    hdr.set_pvname(pvname);
    return hdr;
}

uint64_t expectedEpoch(int year, uint32_t secondsintoyear)
{
    return DateTimeUtils::unixEpochSecondsFromYearAndSecondsIntoYear(year, secondsintoyear);
}

/// Return a pointer to the first DataColumn with the given name, or nullptr.
const DataColumn* findColumn(const DataBatch& batch, const std::string& name)
{
    for (const auto& col : batch.columns)
    {
        if (col.name == name)
            return &col;
    }
    return nullptr;
}

/// Return a pointer to the first EnumDataColumn with the given name, or nullptr.
const EnumDataColumn* findEnumColumn(const DataBatch& batch, const std::string& name)
{
    for (const auto& col : batch.enum_columns)
    {
        if (col.name == name)
            return &col;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// SCALAR_STRING
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarString)
{
    EPICS::ScalarString msg;
    msg.set_secondsintoyear(100);
    msg.set_nano(500);
    msg.set_val("hello");

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_STRING, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.epoch_seconds, expectedEpoch(2024, 100));
    EXPECT_EQ(parsed.nanoseconds, 500u);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& vals = std::get<std::vector<std::string>>(col->values);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], "hello");
}

// ---------------------------------------------------------------------------
// SCALAR_SHORT
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarShort)
{
    EPICS::ScalarShort msg;
    msg.set_secondsintoyear(200);
    msg.set_nano(0);
    msg.set_val(-42);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_SHORT, 2023);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.epoch_seconds, expectedEpoch(2023, 200));
    EXPECT_EQ(parsed.nanoseconds, 0u);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& vals = std::get<std::vector<int32_t>>(col->values);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], -42);
}

// ---------------------------------------------------------------------------
// SCALAR_FLOAT
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarFloat)
{
    EPICS::ScalarFloat msg;
    msg.set_secondsintoyear(300);
    msg.set_nano(999);
    msg.set_val(3.14f);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_FLOAT, 2022);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.epoch_seconds, expectedEpoch(2022, 300));

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& vals = std::get<std::vector<float>>(col->values);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_NEAR(vals[0], 3.14f, 1e-5f);
}

// ---------------------------------------------------------------------------
// SCALAR_ENUM
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarEnum)
{
    EPICS::ScalarEnum msg;
    msg.set_secondsintoyear(10);
    msg.set_nano(1);
    msg.set_val(7);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_ENUM, 2021);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.enum_columns.size(), 1u);
    const auto* col = findEnumColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    ASSERT_EQ(col->values.size(), 1u);
    EXPECT_EQ(col->values[0], 7);
}

// ---------------------------------------------------------------------------
// SCALAR_BYTE (blob)
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarByte)
{
    EPICS::ScalarByte msg;
    msg.set_secondsintoyear(50);
    msg.set_nano(0);
    msg.set_val(std::string("\x01\x02\x03", 3));

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_BYTE, 2020);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "TEST:PV");
    const auto& vals = std::get<std::vector<std::vector<uint8_t>>>(col->values);
    ASSERT_EQ(vals.size(), 1u);
    const std::vector<uint8_t> expected{0x01, 0x02, 0x03};
    EXPECT_EQ(vals[0], expected);
}

// ---------------------------------------------------------------------------
// SCALAR_INT
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarInt)
{
    EPICS::ScalarInt msg;
    msg.set_secondsintoyear(1000);
    msg.set_nano(123456789);
    msg.set_val(0x7FFFFFFF);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_INT, 2019);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.nanoseconds, 123456789u);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& vals = std::get<std::vector<int32_t>>(col->values);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(vals[0], 0x7FFFFFFF);
}

// ---------------------------------------------------------------------------
// SCALAR_DOUBLE
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, ScalarDouble)
{
    EPICS::ScalarDouble msg;
    msg.set_secondsintoyear(86400);
    msg.set_nano(0);
    msg.set_val(2.718281828);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_DOUBLE, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& vals = std::get<std::vector<double>>(col->values);
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_NEAR(vals[0], 2.718281828, 1e-9);
}

// ---------------------------------------------------------------------------
// WAVEFORM_STRING
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformString)
{
    EPICS::VectorString msg;
    msg.set_secondsintoyear(5);
    msg.set_nano(1);
    msg.add_val("alpha");
    msg.add_val("beta");
    msg.add_val("gamma");

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_STRING, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& vals = std::get<std::vector<std::string>>(col->values);
    ASSERT_EQ(vals.size(), 3u);
    EXPECT_EQ(vals[0], "alpha");
    EXPECT_EQ(vals[1], "beta");
    EXPECT_EQ(vals[2], "gamma");
}

// ---------------------------------------------------------------------------
// WAVEFORM_SHORT
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformShort)
{
    EPICS::VectorShort msg;
    msg.set_secondsintoyear(10);
    msg.set_nano(0);
    msg.add_val(1);
    msg.add_val(-2);
    msg.add_val(3);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_SHORT, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& rows = std::get<std::vector<std::vector<int32_t>>>(col->values);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][0], 1);
    EXPECT_EQ(rows[0][1], -2);
    EXPECT_EQ(rows[0][2], 3);
}

// ---------------------------------------------------------------------------
// WAVEFORM_FLOAT
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformFloat)
{
    EPICS::VectorFloat msg;
    msg.set_secondsintoyear(20);
    msg.set_nano(0);
    msg.add_val(1.1f);
    msg.add_val(2.2f);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_FLOAT, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& rows = std::get<std::vector<std::vector<float>>>(col->values);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 2u);
    EXPECT_NEAR(rows[0][0], 1.1f, 1e-5f);
    EXPECT_NEAR(rows[0][1], 2.2f, 1e-5f);
}

// ---------------------------------------------------------------------------
// WAVEFORM_ENUM
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformEnum)
{
    EPICS::VectorEnum msg;
    msg.set_secondsintoyear(30);
    msg.set_nano(0);
    msg.add_val(0);
    msg.add_val(3);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_ENUM, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.enum_columns.size(), 1u);
    const auto* col = findEnumColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    ASSERT_EQ(col->values.size(), 2u);
    EXPECT_EQ(col->values[0], 0);
    EXPECT_EQ(col->values[1], 3);
}

// ---------------------------------------------------------------------------
// WAVEFORM_BYTE (blob via VectorChar)
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformByte)
{
    EPICS::VectorChar msg;
    msg.set_secondsintoyear(40);
    msg.set_nano(77);
    msg.set_val(std::string("\xDE\xAD\xBE\xEF", 4));

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_BYTE, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.nanoseconds, 77u);
    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "TEST:PV");
    const auto& rows = std::get<std::vector<std::vector<uint8_t>>>(col->values);
    ASSERT_EQ(rows.size(), 1u);
    const std::vector<uint8_t> expected{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(rows[0], expected);
}

// ---------------------------------------------------------------------------
// WAVEFORM_INT
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformInt)
{
    EPICS::VectorInt msg;
    msg.set_secondsintoyear(50);
    msg.set_nano(0);
    msg.add_val(100);
    msg.add_val(200);
    msg.add_val(300);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_INT, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& rows = std::get<std::vector<std::vector<int32_t>>>(col->values);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][0], 100);
    EXPECT_EQ(rows[0][1], 200);
    EXPECT_EQ(rows[0][2], 300);
}

// ---------------------------------------------------------------------------
// WAVEFORM_DOUBLE
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, WaveformDouble)
{
    EPICS::VectorDouble msg;
    msg.set_secondsintoyear(60);
    msg.set_nano(0);
    msg.add_val(1.0);
    msg.add_val(2.0);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::WAVEFORM_DOUBLE, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    const auto& rows = std::get<std::vector<std::vector<double>>>(col->values);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 2u);
    EXPECT_DOUBLE_EQ(rows[0][0], 1.0);
    EXPECT_DOUBLE_EQ(rows[0][1], 2.0);
}

// ---------------------------------------------------------------------------
// V4_GENERIC_BYTES
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, V4GenericBytes)
{
    EPICS::V4GenericBytes msg;
    msg.set_secondsintoyear(70);
    msg.set_nano(42);
    msg.set_val(std::string("\xCA\xFE", 2));

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::V4_GENERIC_BYTES, 2024);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.nanoseconds, 42u);
    ASSERT_EQ(parsed.batch.columns.size(), 1u);
    const auto* col = findColumn(parsed.batch, "TEST:PV");
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "TEST:PV");
    const auto& rows = std::get<std::vector<std::vector<uint8_t>>>(col->values);
    ASSERT_EQ(rows.size(), 1u);
    const std::vector<uint8_t> expected{0xCA, 0xFE};
    EXPECT_EQ(rows[0], expected);
}

// ---------------------------------------------------------------------------
// Epoch calculation sanity check
// ---------------------------------------------------------------------------

TEST(ArchiverPbHttpConversionTest, EpochCalculationForKnownDate)
{
    // 2000-01-01 00:00:00 UTC = 946684800 seconds since epoch.
    // Verify by parsing a SCALAR_DOUBLE with year=2000, secondsintoyear=0.
    EPICS::ScalarDouble msg;
    msg.set_secondsintoyear(0);
    msg.set_nano(0);
    msg.set_val(0.0);

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const auto hdr = makeHeader(EPICS::SCALAR_DOUBLE, 2000);
    const auto parsed = ArchiverPbHttpConversion::parseSample(hdr, bytes);

    EXPECT_EQ(parsed.epoch_seconds, 946684800ULL);
}
