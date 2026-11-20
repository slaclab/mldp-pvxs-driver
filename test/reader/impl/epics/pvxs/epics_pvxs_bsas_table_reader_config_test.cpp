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

#include "../../../../config/test_config_helpers.h"

#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderConfig.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::epics::EpicsPVXSBsasTableReaderConfig;
using mldp_pvxs_driver::reader::impl::epics::EpicsReaderConfig;
using Generation = EpicsPVXSBsasTableReaderConfig::Generation;

namespace {

EpicsPVXSBsasTableReaderConfig parse(const std::string& yaml, Generation gen = Generation::Gen2)
{
    return EpicsPVXSBsasTableReaderConfig(makeConfigFromYaml(yaml), gen,
                                          gen == Generation::Gen1 ? "epics-pvxs-bsas-gen1" : "epics-pvxs-bsas-gen2");
}

std::string errorOf(const std::string& yaml, Generation gen = Generation::Gen2)
{
    try
    {
        parse(yaml, gen);
    }
    catch (const EpicsReaderConfig::Error& e)
    {
        return e.what();
    }
    return {};
}

} // namespace

TEST(EpicsPVXSBsasTableReaderConfigTest, Defaults)
{
    const auto cfg = parse(R"(
name: r
pvs:
  - name: T
)");
    EXPECT_EQ(cfg.generation(), Generation::Gen2);
    const auto& o = cfg.optionsFor("T");
    EXPECT_EQ(o.rowsPerDataBatch, 120u);
    EXPECT_EQ(o.pvsPerDataBatch, 1u);
    EXPECT_EQ(o.dataBatchesPerPush, 50u);
    EXPECT_EQ(o.tsSecondsField, "secondsPastEpoch");
    EXPECT_EQ(o.tsNanosField, "nanoseconds");
    EXPECT_EQ(o.pidField, "pid");
    EXPECT_EQ(o.valueSuffix, "Val");
}

TEST(EpicsPVXSBsasTableReaderConfigTest, ReaderLevelKeysApplyToEveryPv)
{
    const auto cfg = parse(R"(
name: r
rows-per-data-batch: 0
pvs-per-data-batch: 0
data-batches-per-push: 7
tsSeconds: s
tsNanos: ns
pidField: pulse
valueSuffix: value
pvs:
  - name: T1
  - name: T2
)");
    for (const char* pv : {"T1", "T2"})
    {
        const auto& o = cfg.optionsFor(pv);
        EXPECT_EQ(o.rowsPerDataBatch, 0u);
        EXPECT_EQ(o.pvsPerDataBatch, 0u);
        EXPECT_EQ(o.dataBatchesPerPush, 7u);
        EXPECT_EQ(o.tsSecondsField, "s");
        EXPECT_EQ(o.tsNanosField, "ns");
        EXPECT_EQ(o.pidField, "pulse");
        EXPECT_EQ(o.valueSuffix, "value");
    }
}

TEST(EpicsPVXSBsasTableReaderConfigTest, PerPvOptionOverridesReaderLevel)
{
    const auto cfg = parse(R"(
name: r
pvs-per-data-batch: 3
rows-per-data-batch: 60
pidField: pulse
pvs:
  - name: T1
    option:
      pvs-per-data-batch: 9
      data-batches-per-push: 4
      tsSeconds: secs
  - name: T2
)");
    const auto& o1 = cfg.optionsFor("T1");
    EXPECT_EQ(o1.pvsPerDataBatch, 9u);
    EXPECT_EQ(o1.dataBatchesPerPush, 4u);
    EXPECT_EQ(o1.rowsPerDataBatch, 60u); // inherited from reader level
    EXPECT_EQ(o1.tsSecondsField, "secs");
    EXPECT_EQ(o1.tsNanosField, "nanoseconds");
    EXPECT_EQ(o1.pidField, "pulse"); // inherited from reader level
    EXPECT_EQ(cfg.optionsFor("T2").pvsPerDataBatch, 3u);
    EXPECT_EQ(cfg.optionsFor("T2").dataBatchesPerPush, 50u);
    EXPECT_EQ(cfg.optionsFor("unknown").pvsPerDataBatch, 3u);
}

TEST(EpicsPVXSBsasTableReaderConfigTest, Gen1AcceptsCommonKeys)
{
    const auto cfg = parse(R"(
name: r
pvs-per-data-batch: 2
pvs:
  - name: T
    option:
      tsNanos: n
)",
                           Generation::Gen1);
    EXPECT_EQ(cfg.generation(), Generation::Gen1);
    EXPECT_EQ(cfg.optionsFor("T").pvsPerDataBatch, 2u);
    EXPECT_EQ(cfg.optionsFor("T").tsNanosField, "n");
}

TEST(EpicsPVXSBsasTableReaderConfigTest, Gen1RejectsGen2Keys)
{
    EXPECT_NE(errorOf("name: r\npidField: pid\n", Generation::Gen1).find("epics-pvxs-bsas-gen2"), std::string::npos);
    EXPECT_FALSE(errorOf(R"(
name: r
pvs:
  - name: T
    option:
      valueSuffix: Val
)",
                         Generation::Gen1)
                     .empty());
}

TEST(EpicsPVXSBsasTableReaderConfigTest, OptionTypeRejected)
{
    const auto err = errorOf(R"(
name: r
pvs:
  - name: T
    option:
      type: slac-bsas-table
)");
    EXPECT_NE(err.find("type"), std::string::npos) << err;
    EXPECT_NE(err.find("implied by the reader type"), std::string::npos) << err;
}

TEST(EpicsPVXSBsasTableReaderConfigTest, ScalarOptionRejected)
{
    EXPECT_FALSE(errorOf(R"(
name: r
pvs:
  - name: T
    option: something
)")
                     .empty());
}

TEST(EpicsPVXSBsasTableReaderConfigTest, EmptyNamesRejected)
{
    for (const char* key : {"tsSeconds", "tsNanos", "pidField", "valueSuffix"})
    {
        EXPECT_FALSE(errorOf(std::string("name: r\n") + key + ": \"\"\n").empty()) << key;
        EXPECT_FALSE(errorOf(std::string("name: r\npvs:\n  - name: T\n    option:\n      ") + key + ": \"\"\n").empty()) << key;
    }
}

TEST(EpicsPVXSBsasTableReaderConfigTest, NonScalarNamesRejected)
{
    EXPECT_FALSE(errorOf("name: r\ntsSeconds: [a, b]\n").empty());
}

TEST(EpicsPVXSBsasTableReaderConfigTest, InvalidCountsRejected)
{
    for (const char* key : {"rows-per-data-batch", "pvs-per-data-batch", "data-batches-per-push"})
    {
        for (const char* value : {"-1", "abc", "1.5", "\"\"", "[1]"})
        {
            const auto err = errorOf(std::string("name: r\n") + key + ": " + value + "\n");
            EXPECT_NE(err.find("non-negative integer"), std::string::npos) << key << "=" << value << ": " << err;
        }
        EXPECT_FALSE(errorOf(std::string("name: r\npvs:\n  - name: T\n    option:\n      ") + key + ": -2\n").empty()) << key;
    }
}

TEST(EpicsPVXSBsasTableReaderConfigTest, ColumnBatchSizeRejectedWithHint)
{
    const auto err = errorOf("name: r\ncolumn-batch-size: 50\n");
    EXPECT_NE(err.find("data-batches-per-push"), std::string::npos) << err;
    EXPECT_FALSE(errorOf("name: r\npvs:\n  - name: T\n    option:\n      column-batch-size: 1\n").empty());
}

TEST(EpicsPVXSBsasTableReaderConfigTest, BaseValidationStillApplies)
{
    EXPECT_FALSE(errorOf("pvs:\n  - name: T\n").empty()); // missing name
    EXPECT_FALSE(errorOf("name: r\npvs:\n  - option: {}\n").empty()); // pv without name
}

namespace {

std::filesystem::path writePvListFile(const std::string& content)
{
    std::random_device rd;
    const auto         path = std::filesystem::temp_directory_path() /
                      ("mldp_bsas_pvs_" + std::to_string(rd()) + "_" + std::to_string(rd()) + ".txt");
    std::ofstream out(path);
    out << content;
    return path;
}

} // namespace

TEST(EpicsPVXSBsasTableReaderConfigTest, PvsFilesOptionAppliesToEveryPvGen1)
{
    const auto file = writePvListFile("G1:A\nG1:B\n");
    const auto cfg = parse("name: r\ndata-batches-per-push: 50\npvs:\n  - name: INLINE\npvs-files:\n  - path: " +
                               file.string() + "\n    option:\n      data-batches-per-push: 4\n      rows-per-data-batch: 10\n",
                           Generation::Gen1);
    std::filesystem::remove(file);

    EXPECT_EQ(cfg.optionsFor("INLINE").dataBatchesPerPush, 50u);
    for (const char* pv : {"G1:A", "G1:B"})
    {
        EXPECT_EQ(cfg.optionsFor(pv).dataBatchesPerPush, 4u) << pv;
        EXPECT_EQ(cfg.optionsFor(pv).rowsPerDataBatch, 10u) << pv;
    }
}

TEST(EpicsPVXSBsasTableReaderConfigTest, PvsFilesOptionAppliesToEveryPvGen2)
{
    const auto file = writePvListFile("G2:A\nG2:B\n");
    const auto cfg = parse("name: r\npvs-files:\n  - path: " + file.string() +
                           "\n    option:\n      pvs-per-data-batch: 0\n      valueSuffix: VAL\n");
    std::filesystem::remove(file);

    ASSERT_EQ(cfg.pvNames().size(), 2u);
    for (const char* pv : {"G2:A", "G2:B"})
    {
        EXPECT_EQ(cfg.optionsFor(pv).pvsPerDataBatch, 0u) << pv;
        EXPECT_EQ(cfg.optionsFor(pv).valueSuffix, "VAL") << pv;
    }
}

TEST(EpicsPVXSBsasTableReaderConfigTest, PvsFilesOptionRejectsTypeAndColumnBatchSize)
{
    const auto file = writePvListFile("T:1\n");
    const auto base = "name: r\npvs-files:\n  - path: " + file.string() + "\n    option:\n";
    EXPECT_NE(errorOf(base + "      type: slac-bsas-table\n", Generation::Gen1).find("type"), std::string::npos);
    EXPECT_NE(errorOf(base + "      column-batch-size: 4\n", Generation::Gen1).find("data-batches-per-push"), std::string::npos);
    std::filesystem::remove(file);
}

TEST(EpicsPVXSBsasTableReaderConfigTest, Gen1SignalPvDerivedFromTblSuffixOrOverridden)
{
    const auto cfg = parse(R"(
name: r
signal-timeout-ms: 2000
pvs:
  - name: BSAS:SYS0:1:CUSXR_TBL
  - name: OTHER:TABLE
  - name: X_TBL
    option:
      signal-pv: CUSTOM:NAMES
)", Generation::Gen1);
    EXPECT_EQ(cfg.optionsFor("BSAS:SYS0:1:CUSXR_TBL").signalPv, "BSAS:SYS0:1:CUSXR_SIG");
    EXPECT_EQ(cfg.optionsFor("BSAS:SYS0:1:CUSXR_TBL").signalTimeoutMs, 2000u);
    EXPECT_EQ(cfg.optionsFor("OTHER:TABLE").signalPv, "");
    EXPECT_EQ(cfg.optionsFor("X_TBL").signalPv, "CUSTOM:NAMES");
}

TEST(EpicsPVXSBsasTableReaderConfigTest, SignalKeysValidation)
{
    EXPECT_NE(errorOf("name: r\nsignal-pv: X\npvs:\n  - name: T_TBL\n", Generation::Gen1).find("per PV"), std::string::npos);
    EXPECT_NE(errorOf("name: r\nsignal-timeout-ms: 0\npvs:\n  - name: T_TBL\n", Generation::Gen1).find("greater than 0"),
              std::string::npos);
    EXPECT_NE(errorOf("name: r\npvs:\n  - name: T\n    option:\n      signal-pv: X\n").find("gen1"), std::string::npos);
}
