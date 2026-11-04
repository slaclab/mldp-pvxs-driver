//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// Tests for the epics-pvxs-bsas-gen2 reader: column filter unit tests,
// in-process conversion tests and end-to-end tests against the mock IOC Gen2
// tables (test/mock/sioc.cpp, test:bsas_gen2_*).

#include <gtest/gtest.h>

#include "../../../../config/test_config_helpers.h"
#include "../../../../mock/sioc.h"
#include "bsas_table_test_utils.h"

#include <metrics/Metrics.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/epics/pvxs/BSASEpicsDataBatchConversion.h>
#include <reader/impl/epics/pvxs/EpicsPVXSBsasGen2Reader.h>
#include <util/log/ILog.h>

#include <pvxs/nt.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bsas_table_test_utils;
using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::ReaderFactory;
using mldp_pvxs_driver::reader::impl::epics::BSASEpicsDataBatchConversion;
using mldp_pvxs_driver::reader::impl::epics::EpicsPVXSBsasGen2Reader;
using mldp_pvxs_driver::reader::impl::epics::EpicsPVXSBsasTableReaderConfig;
using Opts = EpicsPVXSBsasTableReaderConfig::TableOptions;

namespace {

constexpr const char* kReaderType = "epics-pvxs-bsas-gen2";

const std::vector<std::string> kExpectedPvs{"TST:GEN2:A", "TST:GEN2:B"};

/// Mock update counter of a group, recovered from its row timestamp.
int counterOf(const DataBatch& frame)
{
    return static_cast<int>(frame.timestamps.at(0).epoch_seconds - BsasTablePV::kBaseSeconds);
}

/// Expect Val columns of the mock (A: Float64, B: Float32 + 50) for a group.
void expectMockValColumns(const Group& g, const std::vector<std::string>& names)
{
    ASSERT_EQ(g.columnNames(), names);

    const DataBatch* frameA = nullptr;
    const auto*      a = g.column(names[0], &frameA);
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(frameA->timestamps.size(), 4u);
    for (std::size_t r = 0; r < 4; ++r)
    {
        EXPECT_EQ(frameA->timestamps[r].nanoseconds, r * BsasTablePV::kRowNanosStep);
    }
    const double base = counterOf(*frameA) * 100.0;
    ASSERT_TRUE(std::holds_alternative<std::vector<double>>(a->values));
    EXPECT_EQ(asDoubles(*a), (std::vector<double>{base, base + 1, base + 2, base + 3}));

    const DataBatch* frameB = nullptr;
    const auto*      b = g.column(names[1], &frameB);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(sameTimestamps(*frameA, *frameB));
    ASSERT_TRUE(std::holds_alternative<std::vector<float>>(b->values));
    EXPECT_EQ(asDoubles(*b), (std::vector<double>{base + 50, base + 51, base + 52, base + 53}));
}

// ---------------------------------------------------------------------------
// In-process NTTable builder for conversion tests (no IOC).
// ---------------------------------------------------------------------------

struct LocalColumn
{
    std::string         field;
    std::string         label;
    std::vector<double> values;
};

pvxs::Value makeGen2Table(const std::vector<LocalColumn>& cols, bool withLabels = true)
{
    pvxs::nt::NTTable builder;
    for (const auto& c : cols)
    {
        builder.add_column(pvxs::TypeCode::Float64, c.field.c_str());
    }
    builder.add_column(pvxs::TypeCode::UInt32, "secondsPastEpoch");
    builder.add_column(pvxs::TypeCode::UInt32, "nanoseconds");
    auto value = builder.build().create();

    const std::size_t rows = cols.empty() ? 0 : cols.front().values.size();
    if (withLabels)
    {
        pvxs::shared_array<std::string> labels(cols.size() + 2);
        for (std::size_t i = 0; i < cols.size(); ++i)
        {
            labels[i] = cols[i].label;
        }
        labels[cols.size()] = "secondsPastEpoch";
        labels[cols.size() + 1] = "nanoseconds";
        value["labels"] = labels.freeze();
    }
    for (const auto& c : cols)
    {
        pvxs::shared_array<double> arr(c.values.begin(), c.values.end());
        value["value"][c.field].from(arr.freeze());
    }
    pvxs::shared_array<uint32_t> secs(rows), nanos(rows);
    for (std::size_t r = 0; r < rows; ++r)
    {
        secs[r] = 100;
        nanos[r] = static_cast<uint32_t>(r);
    }
    value["value.secondsPastEpoch"].from(secs.freeze());
    value["value.nanoseconds"].from(nanos.freeze());
    return value;
}

std::vector<LocalColumn> standardLocalColumns()
{
    return {
        {"pid", "pid", {1, 2}},
        {"X_Y_Val", "X:Y.Val", {10, 11}},
        {"X_Y_CNT", "X:Y.CNT", {-1, -1}},
        {"X_Y_RMS", "X:Y.RMS", {-2, -2}},
        {"Z_W_Val", "Z:W.Val", {20, 21}},
        {"Z_W_MAX", "Z:W.MAX", {-3, -3}},
    };
}

/// Run the sequential conversion with the Gen2 filter; returns (name, values).
std::vector<std::pair<std::string, std::vector<double>>> convertWithGen2Filter(const pvxs::Value& table,
                                                                               const Opts&        opts,
                                                                               BS::light_thread_pool* pool = nullptr)
{
    auto        logger = mldp_pvxs_driver::util::log::newLogger("bsas_gen2_test");
    std::size_t emitted = 0;
    std::vector<std::pair<std::string, std::vector<double>>> out;
    BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch(
        *logger, "LOCAL:TABLE", table, opts.tsSecondsField, opts.tsNanosField,
        [&](std::string name, std::vector<DataBatch> batches)
        {
            for (const auto& b : batches)
            {
                for (const auto& c : b.columns)
                {
                    EXPECT_EQ(c.name, name) << "DataColumn::name must match the emitted column name";
                    out.emplace_back(c.name, asDoubles(c));
                }
            }
        },
        emitted, pool, EpicsPVXSBsasGen2Reader::makeColumnFilter(opts));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Column filter (no IOC)
// ---------------------------------------------------------------------------

TEST(EpicsPVXSBsasGen2FilterTest, KeepsValColumnsAndStripsSuffix)
{
    const Opts opts;
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("XCOR:LI28:202:BACT.Val", opts), "XCOR:LI28:202:BACT");
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("PV.Val", opts), "PV");
}

TEST(EpicsPVXSBsasGen2FilterTest, SuffixMatchIsCaseInsensitive)
{
    const Opts opts;
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("PV.VAL", opts), "PV");
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("PV.val", opts), "PV");
}

TEST(EpicsPVXSBsasGen2FilterTest, DropsStatisticsColumns)
{
    const Opts opts;
    for (const char* suffix : {"CNT", "RMS", "AVG", "MIN", "MAX"})
    {
        EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName(std::string("PV:A.") + suffix, opts).has_value()) << suffix;
    }
}

TEST(EpicsPVXSBsasGen2FilterTest, DropsPidAndTimestampColumns)
{
    const Opts opts;
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("pid", opts).has_value());
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("secondsPastEpoch", opts).has_value());
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("nanoseconds", opts).has_value());
}

TEST(EpicsPVXSBsasGen2FilterTest, FallsBackToUnderscoreSeparator)
{
    const Opts opts;
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("XCOR_LI28_202_BACT_Val", opts), "XCOR_LI28_202_BACT");
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("XCOR_LI28_202_BACT_CNT", opts).has_value());
}

TEST(EpicsPVXSBsasGen2FilterTest, DotTakesPrecedenceOverUnderscore)
{
    const Opts opts;
    // Last '.' wins even when the base name contains '_'.
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("PV_X.Val", opts), "PV_X");
    // A '.' is present, so the trailing "_Val" is not a separator here.
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("PV.X_Val", opts).has_value());
}

TEST(EpicsPVXSBsasGen2FilterTest, RejectsLabelsWithoutSeparatorOrBase)
{
    const Opts opts;
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("Val", opts).has_value());
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName(".Val", opts).has_value());
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("_Val", opts).has_value());
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("PV.", opts).has_value());
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("", opts).has_value());
}

TEST(EpicsPVXSBsasGen2FilterTest, HonoursCustomPidAndSuffix)
{
    Opts opts;
    opts.pidField = "pulseId";
    opts.valueSuffix = "value";
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("pulseId", opts).has_value());
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("PV:A_value", opts), "PV:A");
    EXPECT_EQ(EpicsPVXSBsasGen2Reader::valueColumnName("PV:A.VALUE", opts), "PV:A");
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("PV:A.Val", opts).has_value());
    // "pid" is no longer special but still has no value suffix.
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("pid", opts).has_value());
}

TEST(EpicsPVXSBsasGen2FilterTest, HonoursCustomTimestampNames)
{
    Opts opts;
    opts.tsSecondsField = "secs.Val";
    EXPECT_FALSE(EpicsPVXSBsasGen2Reader::valueColumnName("secs.Val", opts).has_value());
}

// ---------------------------------------------------------------------------
// Conversion with filter (no IOC)
// ---------------------------------------------------------------------------

TEST(EpicsPVXSBsasGen2ConversionTest, UsesLabelsForNamesAndConvertsValColumnsOnly)
{
    const auto out = convertWithGen2Filter(makeGen2Table(standardLocalColumns()), Opts{});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, "X:Y");
    EXPECT_EQ(out[0].second, (std::vector<double>{10, 11}));
    EXPECT_EQ(out[1].first, "Z:W");
    EXPECT_EQ(out[1].second, (std::vector<double>{20, 21}));
}

TEST(EpicsPVXSBsasGen2ConversionTest, FallsBackToFieldNamesWithoutLabels)
{
    const auto out = convertWithGen2Filter(makeGen2Table(standardLocalColumns(), /*withLabels=*/false), Opts{});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, "X_Y");
    EXPECT_EQ(out[1].first, "Z_W");
}

TEST(EpicsPVXSBsasGen2ConversionTest, ParallelConversionMatchesSequential)
{
    BS::light_thread_pool pool(4);
    const auto            table = makeGen2Table(standardLocalColumns());
    EXPECT_EQ(convertWithGen2Filter(table, Opts{}, &pool), convertWithGen2Filter(table, Opts{}));
}

TEST(EpicsPVXSBsasGen2ConversionTest, NoMatchingColumnReturnsFalse)
{
    auto        logger = mldp_pvxs_driver::util::log::newLogger("bsas_gen2_test");
    std::size_t emitted = 0;
    std::size_t calls = 0;
    const bool  ok = BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch(
        *logger, "LOCAL:TABLE", makeGen2Table({{"pid", "pid", {1}}, {"A_CNT", "A.CNT", {1}}}),
        "secondsPastEpoch", "nanoseconds",
        [&](std::string, std::vector<DataBatch>) { ++calls; },
        emitted, EpicsPVXSBsasGen2Reader::makeColumnFilter(Opts{}));
    EXPECT_FALSE(ok);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(emitted, 0u);
}

TEST(EpicsPVXSBsasGen2ConversionTest, NullFilterKeepsEveryDataColumn)
{
    auto                     logger = mldp_pvxs_driver::util::log::newLogger("bsas_gen2_test");
    std::size_t              emitted = 0;
    std::vector<std::string> names;
    ASSERT_TRUE(BSASEpicsDataBatchConversion::tryBuildNtTableRowTsBatch(
        *logger, "LOCAL:TABLE", makeGen2Table(standardLocalColumns()), "secondsPastEpoch", "nanoseconds",
        [&](std::string name, std::vector<DataBatch>) { names.push_back(name); },
        emitted));
    EXPECT_EQ(names, (std::vector<std::string>{"pid", "X_Y_Val", "X_Y_CNT", "X_Y_RMS", "Z_W_Val", "Z_W_MAX"}));
    EXPECT_EQ(emitted, 12u);
}

// ---------------------------------------------------------------------------
// End-to-end against the mock IOC
// ---------------------------------------------------------------------------

class EpicsPVXSBsasGen2ReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus = std::make_shared<RecordingBus>();
        server = std::make_shared<PVServer>();
    }

    void TearDown() override
    {
        reader.reset();
        server.reset();
        bus.reset();
    }

    void start(const std::string& yaml, std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics = nullptr)
    {
        reader = ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(yaml), metrics);
        ASSERT_NE(reader, nullptr);
    }

    std::vector<Group> waitForDataGroups(const std::string& pv, std::size_t count)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto groups = dataGroupsFor(bus->snapshot(), pv);
            if (groups.size() >= count)
            {
                return groups;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return dataGroupsFor(bus->snapshot(), pv);
    }

    std::shared_ptr<RecordingBus>                     bus;
    std::shared_ptr<PVServer>                         server;
    std::shared_ptr<mldp_pvxs_driver::reader::Reader> reader;
};

TEST_F(EpicsPVXSBsasGen2ReaderTest, FactoryCreatesReaderWithName)
{
    start(R"(
name: bsas_gen2
)");
    EXPECT_EQ(reader->name(), "bsas_gen2");
    EXPECT_NE(std::dynamic_pointer_cast<EpicsPVXSBsasGen2Reader>(reader), nullptr);
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, EmitsOnlyValColumnsRenamedToPvNames)
{
    start(R"(
name: bsas_gen2
pvs:
  - name: test:bsas_gen2_table
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_table", 2);
    ASSERT_GE(groups.size(), 2u);
    for (const auto& g : groups)
    {
        // No pid, no CNT/RMS/AVG/MIN/MAX: Val columns only, named from labels.
        expectMockValColumns(g, kExpectedPvs);
        for (const auto& b : g.data)
        {
            const auto& ts = std::get<TimeSeriesPayload>(b.payload);
            EXPECT_TRUE(ts.is_tabular);
            EXPECT_EQ(ts.root_source_name, "test:bsas_gen2_table");
            EXPECT_EQ(b.reader_name, "bsas_gen2");
        }
        const auto& marker = std::get<TimeSeriesPayload>(g.marker.payload);
        EXPECT_TRUE(marker.end_of_batch_group);
        EXPECT_TRUE(marker.is_tabular);
    }
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, FallsBackToSanitizedFieldNamesWithoutLabels)
{
    start(R"(
name: bsas_gen2
pvs:
  - name: test:bsas_gen2_no_labels
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_no_labels", 1);
    ASSERT_GE(groups.size(), 1u);
    expectMockValColumns(groups.front(), {"TST_GEN2_A", "TST_GEN2_B"});
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, PvsPerDataBatchGroupsValColumns)
{
    start(R"(
name: bsas_gen2
pvs-per-data-batch: 0
pvs:
  - name: test:bsas_gen2_table
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_table", 1);
    ASSERT_GE(groups.size(), 1u);
    const auto frames = groups.front().frames();
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0]->columns.size(), 2u);
    EXPECT_EQ(frames[0]->columns[0].name, "TST:GEN2:A");
    EXPECT_EQ(frames[0]->columns[1].name, "TST:GEN2:B");
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, AllNaNValColumnIsDropped)
{
    start(R"(
name: bsas_gen2
pvs:
  - name: test:bsas_gen2_nan
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_nan", 1);
    ASSERT_GE(groups.size(), 1u);
    // A.Val is all-NaN (its statistics are valid but never forwarded).
    EXPECT_EQ(groups.front().columnNames(), (std::vector<std::string>{"TST:GEN2:B"}));
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, CustomPidFieldAndValueSuffix)
{
    start(R"(
name: bsas_gen2
pidField: pulseId
valueSuffix: VALUE
pvs:
  - name: test:bsas_gen2_custom
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_custom", 1);
    ASSERT_GE(groups.size(), 1u);
    // Labels use '_' separator and lowercase "value"; suffix matched case-insensitively.
    expectMockValColumns(groups.front(), kExpectedPvs);
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, PerPvOptionsOverrideReaderLevel)
{
    start(R"(
name: bsas_gen2
pvs:
  - name: test:bsas_gen2_table
  - name: test:bsas_gen2_custom
    option:
      pidField: pulseId
      valueSuffix: value
)");
    const auto std_groups = waitForDataGroups("test:bsas_gen2_table", 1);
    const auto custom_groups = waitForDataGroups("test:bsas_gen2_custom", 1);
    ASSERT_GE(std_groups.size(), 1u);
    ASSERT_GE(custom_groups.size(), 1u);
    expectMockValColumns(std_groups.front(), kExpectedPvs);
    expectMockValColumns(custom_groups.front(), kExpectedPvs);
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, DefaultSuffixMatchesNothingInCustomTable)
{
    auto metrics = std::make_shared<mldp_pvxs_driver::metrics::Metrics>(mldp_pvxs_driver::metrics::MetricsConfig());
    start(R"(
name: bsas_gen2
pvs:
  - name: test:bsas_gen2_custom
)",
          metrics);
    // Default suffix "Val" never matches "_value": no data, markers only.
    ASSERT_TRUE(bus->waitForMarkers("test:bsas_gen2_custom", 2));
    EXPECT_TRUE(dataGroupsFor(bus->snapshot(), "test:bsas_gen2_custom").empty());
    EXPECT_GE(metrics->readerErrorsTotal(prometheus::Labels{{"source", "test:bsas_gen2_custom"}}), 2.0);
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, ParallelConversionGivesSameColumns)
{
    start(R"(
name: bsas_gen2
thread-pool: 4
pvs:
  - name: test:bsas_gen2_table
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_table", 2);
    ASSERT_GE(groups.size(), 2u);
    for (const auto& g : groups)
    {
        expectMockValColumns(g, kExpectedPvs);
    }
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, MetadataMerged)
{
    start(R"(
name: bsas_gen2
metadata:
  facility: lcls
pvs:
  - name: test:bsas_gen2_table
    metadata:
      line: sxr
)");
    const auto groups = waitForDataGroups("test:bsas_gen2_table", 1);
    ASSERT_GE(groups.size(), 1u);
    for (const auto& b : groups.front().data)
    {
        EXPECT_EQ(b.metadata.at("facility"), "lcls");
        EXPECT_EQ(b.metadata.at("line"), "sxr");
        EXPECT_EQ(b.metadata.at("source"), "test:bsas_gen2_table");
    }
}

TEST_F(EpicsPVXSBsasGen2ReaderTest, OptionTypeIsRejected)
{
    EXPECT_ANY_THROW(ReaderFactory::create(kReaderType, bus, makeConfigFromYaml(R"(
name: bsas_gen2
pvs:
  - name: test:bsas_gen2_table
    option:
      type: slac-bsas-table
)")));
}
