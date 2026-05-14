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

#include <config/wizard.h>
#include <config/validate.h>
#include "wizard_internal.h"
#include "test_config_helpers.h"

#include <fstream>
#include <string>
#include <filesystem>

namespace mldp_pvxs_driver::config {

using namespace wizard_internal;

// ─────────────────────────────────────────────────────────────────────────────
// Validator helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST(WizardValidators, Iso8601AcceptsValidForms)
{
    EXPECT_TRUE(isValidIso8601("2026-01-01"));
    EXPECT_TRUE(isValidIso8601("2026-01-01T00:00:00Z"));
    EXPECT_TRUE(isValidIso8601("2026-01-01T08:30:00+05:30"));
    EXPECT_TRUE(isValidIso8601("2026-12-31T23:59:59-07:00"));
}

TEST(WizardValidators, Iso8601RejectsInvalidForms)
{
    EXPECT_FALSE(isValidIso8601(""));
    EXPECT_FALSE(isValidIso8601("not-a-date"));
    EXPECT_FALSE(isValidIso8601("2026/01/01"));
    EXPECT_FALSE(isValidIso8601("01-01-2026"));
    EXPECT_FALSE(isValidIso8601("2026-1-1"));
}

TEST(WizardValidators, IsPositiveInt)
{
    EXPECT_TRUE(isPositiveInt("1"));
    EXPECT_TRUE(isPositiveInt("42"));
    EXPECT_FALSE(isPositiveInt("0"));
    EXPECT_FALSE(isPositiveInt("-1"));
    EXPECT_FALSE(isPositiveInt(""));
    EXPECT_FALSE(isPositiveInt("abc"));
    EXPECT_FALSE(isPositiveInt("1.5"));
}

TEST(WizardValidators, IsNonNegInt)
{
    EXPECT_TRUE(isNonNegInt("0"));
    EXPECT_TRUE(isNonNegInt("1"));
    EXPECT_TRUE(isNonNegInt("300"));
    EXPECT_FALSE(isNonNegInt(""));
    EXPECT_FALSE(isNonNegInt("-1"));
    EXPECT_FALSE(isNonNegInt("abc"));
}

// ─────────────────────────────────────────────────────────────────────────────
// generateYaml — structural correctness
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a minimal valid WizardState (one mldp writer + one pvxs reader)
static WizardState makeMinimalState()
{
    WizardState st;
    st.controller_name = "test_ctrl";

    MldpWriterConfig w;
    w.name           = "mldp_main";
    w.provider_name  = "pvxs_provider";
    w.ingestion_url  = "grpc://mldp-ingest.example.com:50051";
    st.mldp_writers.push_back(w);

    EpicsReaderConfig r;
    r.name        = "pvxs_reader";
    r.reader_type = "epics-pvxs";
    st.readers.push_back(r);

    return st;
}

TEST(WizardGenerateYaml, ContainsControllerName)
{
    auto st  = makeMinimalState();
    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("name: test_ctrl"));
}

TEST(WizardGenerateYaml, ContainsMldpWriter)
{
    auto st   = makeMinimalState();
    auto yaml  = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("mldp:"));
    EXPECT_NE(std::string::npos, yaml.find("name: mldp_main"));
    EXPECT_NE(std::string::npos, yaml.find("ingestion-url: grpc://mldp-ingest.example.com:50051"));
}

TEST(WizardGenerateYaml, NoPvsBlockWhenReaderHasNoPvs)
{
    auto st   = makeMinimalState();
    auto yaml  = generateYaml(st);
    // reader has no pvs — pvs: block must not appear
    EXPECT_EQ(std::string::npos, yaml.find("pvs:"));
}

TEST(WizardGenerateYaml, PvsBlockPresentWhenReaderHasPvs)
{
    auto st = makeMinimalState();
    PvEntry pv;
    pv.name        = "SITE:SYS:PRESSURE";
    pv.option_type = "none";
    st.readers[0].pvs.push_back(pv);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("pvs:"));
    EXPECT_NE(std::string::npos, yaml.find("name: SITE:SYS:PRESSURE"));
}

TEST(WizardGenerateYaml, ScalarPvOption)
{
    auto st = makeMinimalState();
    PvEntry pv;
    pv.name         = "SITE:SYS:BEAM";
    pv.option_type  = "scalar";
    pv.option_value = "chan://beam";
    st.readers[0].pvs.push_back(pv);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("option: \"chan://beam\""));
}

TEST(WizardGenerateYaml, SlacBsasTablePvOption)
{
    auto st = makeMinimalState();
    PvEntry pv;
    pv.name        = "SITE:SYS:TABLE";
    pv.option_type = "slac-bsas-table";
    pv.ts_seconds  = "secondsPastEpoch";
    pv.ts_nanos    = "nanoseconds";
    st.readers[0].pvs.push_back(pv);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("type: slac-bsas-table"));
    EXPECT_NE(std::string::npos, yaml.find("tsSeconds: secondsPastEpoch"));
    EXPECT_NE(std::string::npos, yaml.find("tsNanos: nanoseconds"));
}

TEST(WizardGenerateYaml, Hdf5Writer)
{
    WizardState st;
    st.controller_name = "default";

    Hdf5WriterConfig hw;
    hw.name              = "hdf5_local";
    hw.base_path         = "/data/hdf5";
    hw.compression_level = "4";
    hw.is_merge          = false;
    st.hdf5_writers.push_back(hw);

    EpicsReaderConfig r;
    r.name        = "pvxs_r";
    r.reader_type = "epics-pvxs";
    st.readers.push_back(r);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("hdf5:"));
    EXPECT_NE(std::string::npos, yaml.find("name: hdf5_local"));
    EXPECT_NE(std::string::npos, yaml.find("base-path: /data/hdf5"));
    EXPECT_NE(std::string::npos, yaml.find("compression-level: 4"));
    EXPECT_EQ(std::string::npos, yaml.find("hdf5-merge:"));
}

TEST(WizardGenerateYaml, Hdf5MergeWriter)
{
    WizardState st;
    st.controller_name = "default";

    Hdf5WriterConfig hw;
    hw.name     = "hdf5_merge";
    hw.base_path = "/data/merge";
    hw.is_merge  = true;
    st.hdf5_writers.push_back(hw);

    EpicsReaderConfig r;
    r.name        = "pvxs_r";
    r.reader_type = "epics-pvxs";
    st.readers.push_back(r);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("hdf5-merge:"));
    EXPECT_EQ(std::string::npos, yaml.find("hdf5:\n"));
}

TEST(WizardGenerateYaml, MetricsEmittedWhenEnabled)
{
    auto st            = makeMinimalState();
    st.metrics_enabled  = true;
    st.metrics_endpoint = "0.0.0.0:9464";
    st.metrics_interval = "5";

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("metrics:"));
    EXPECT_NE(std::string::npos, yaml.find("endpoint: \"0.0.0.0:9464\""));
    EXPECT_NE(std::string::npos, yaml.find("scan-interval-seconds: 5"));
}

TEST(WizardGenerateYaml, MetricsOmittedWhenDisabled)
{
    auto st             = makeMinimalState();
    st.metrics_enabled  = false;
    auto yaml           = generateYaml(st);
    EXPECT_EQ(std::string::npos, yaml.find("metrics:"));
}

TEST(WizardGenerateYaml, RoutingAllToAllOmitsRoutingBlock)
{
    auto st               = makeMinimalState();
    st.routing_all_to_all = true;
    auto yaml             = generateYaml(st);
    EXPECT_EQ(std::string::npos, yaml.find("routing:"));
}

TEST(WizardGenerateYaml, ExplicitRoutingEmitted)
{
    auto st               = makeMinimalState();
    st.routing_all_to_all = false;

    RoutingEntry re;
    re.writer_name   = "mldp_main";
    re.from_readers  = {"pvxs_reader"};
    re.include_globs = {"SITE:BPM:*"};
    re.exclude_globs = {"SITE:BPM:OLD:*"};
    st.routing.push_back(re);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("routing:"));
    EXPECT_NE(std::string::npos, yaml.find("mldp_main:"));
    EXPECT_NE(std::string::npos, yaml.find("pvxs_reader"));
    EXPECT_NE(std::string::npos, yaml.find("\"SITE:BPM:*\""));
    EXPECT_NE(std::string::npos, yaml.find("\"SITE:BPM:OLD:*\""));
}

TEST(WizardGenerateYaml, ArchiverReaderHistoricalOnce)
{
    auto st = makeMinimalState();

    EpicsReaderConfig arch;
    arch.name        = "archiver_hist";
    arch.reader_type = "epics-archiver";
    arch.hostname    = "archiver.example.com:11200";
    arch.mode        = "historical_once";
    arch.start_date  = "2026-01-01T00:00:00Z";
    arch.end_date    = "2026-01-02T00:00:00Z";
    st.readers.push_back(arch);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("epics-archiver:"));
    EXPECT_NE(std::string::npos, yaml.find("mode: historical_once"));
    EXPECT_NE(std::string::npos, yaml.find("start-date:"));
    EXPECT_NE(std::string::npos, yaml.find("2026-01-01T00:00:00Z"));
    EXPECT_EQ(std::string::npos, yaml.find("poll-interval-sec:"));
}

TEST(WizardGenerateYaml, ArchiverReaderPeriodicTail)
{
    auto st = makeMinimalState();

    EpicsReaderConfig arch;
    arch.name              = "archiver_tail";
    arch.reader_type       = "epics-archiver";
    arch.hostname          = "archiver.example.com:11200";
    arch.mode              = "periodic_tail";
    arch.poll_interval_sec = "10";
    arch.lookback_sec      = "10";
    st.readers.push_back(arch);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("mode: periodic_tail"));
    EXPECT_NE(std::string::npos, yaml.find("poll-interval-sec: 10"));
    EXPECT_EQ(std::string::npos, yaml.find("start-date:"));
}

TEST(WizardGenerateYaml, MldpCredentialsCustomTls)
{
    WizardState st;
    st.controller_name = "default";

    MldpWriterConfig w;
    w.name           = "mldp_tls";
    w.provider_name  = "prov";
    w.ingestion_url  = "grpc://host:50051";
    w.creds_type     = "custom-tls";
    w.pem_cert_chain  = "/etc/ssl/cert.pem";
    w.pem_private_key = "/etc/ssl/key.pem";
    w.pem_root_certs  = "/etc/ssl/root.pem";
    st.mldp_writers.push_back(w);

    EpicsReaderConfig r;
    r.name        = "pvxs_r";
    r.reader_type = "epics-pvxs";
    st.readers.push_back(r);

    auto yaml = generateYaml(st);
    EXPECT_NE(std::string::npos, yaml.find("pem-cert-chain: /etc/ssl/cert.pem"));
    EXPECT_NE(std::string::npos, yaml.find("pem-private-key: /etc/ssl/key.pem"));
    EXPECT_NE(std::string::npos, yaml.find("pem-root-certs: /etc/ssl/root.pem"));
}

// ─────────────────────────────────────────────────────────────────────────────
// generateYaml → validateConfig round-trip
// Output of generateYaml must pass validateConfig with 0 errors.
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<ConfigDiagnostic> validateYamlString(const std::string& yaml)
{
    auto cfg = makeConfigFromYaml(yaml);
    return validateConfig(cfg);
}

static int countErrors(const std::vector<ConfigDiagnostic>& diags)
{
    int n = 0;
    for (const auto& d : diags)
        if (d.severity == ConfigDiagnostic::Severity::ERROR) ++n;
    return n;
}

TEST(WizardRoundTrip, MinimalMldpPvxsPassesValidation)
{
    auto st   = makeMinimalState();
    auto yaml = generateYaml(st);
    auto diags = validateYamlString(yaml);
    EXPECT_EQ(0, countErrors(diags)) << yaml;
}

TEST(WizardRoundTrip, Hdf5WriterPassesValidation)
{
    WizardState st;
    st.controller_name = "default";

    Hdf5WriterConfig hw;
    hw.name      = "hdf5_local";
    hw.base_path = "/data/hdf5";
    st.hdf5_writers.push_back(hw);

    EpicsReaderConfig r;
    r.name        = "pvxs_r";
    r.reader_type = "epics-pvxs";
    st.readers.push_back(r);

    auto yaml  = generateYaml(st);
    auto diags = validateYamlString(yaml);
    EXPECT_EQ(0, countErrors(diags)) << yaml;
}

TEST(WizardRoundTrip, ArchiverHistoricalOncePassesValidation)
{
    auto st = makeMinimalState();

    EpicsReaderConfig arch;
    arch.name        = "archiver_hist";
    arch.reader_type = "epics-archiver";
    arch.hostname    = "archiver.example.com:11200";
    arch.mode        = "historical_once";
    arch.start_date  = "2026-01-01T00:00:00Z";
    st.readers.push_back(arch);

    auto yaml  = generateYaml(st);
    auto diags = validateYamlString(yaml);
    EXPECT_EQ(0, countErrors(diags)) << yaml;
}

TEST(WizardRoundTrip, ArchiverPeriodicTailPassesValidation)
{
    auto st = makeMinimalState();

    EpicsReaderConfig arch;
    arch.name              = "archiver_tail";
    arch.reader_type       = "epics-archiver";
    arch.hostname          = "archiver.example.com:11200";
    arch.mode              = "periodic_tail";
    arch.poll_interval_sec = "10";
    st.readers.push_back(arch);

    auto yaml  = generateYaml(st);
    auto diags = validateYamlString(yaml);
    EXPECT_EQ(0, countErrors(diags)) << yaml;
}

TEST(WizardRoundTrip, ExplicitRoutingPassesValidation)
{
    auto st               = makeMinimalState();
    st.routing_all_to_all = false;

    RoutingEntry re;
    re.writer_name  = "mldp_main";
    re.from_readers = {"pvxs_reader"};
    st.routing.push_back(re);

    auto yaml  = generateYaml(st);
    auto diags = validateYamlString(yaml);
    EXPECT_EQ(0, countErrors(diags)) << yaml;
}

TEST(WizardRoundTrip, MetricsEnabledPassesValidation)
{
    auto st             = makeMinimalState();
    st.metrics_enabled  = true;
    st.metrics_endpoint = "0.0.0.0:9464";
    st.metrics_interval = "1";

    auto yaml  = generateYaml(st);
    auto diags = validateYamlString(yaml);
    EXPECT_EQ(0, countErrors(diags)) << yaml;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadFromConfig round-trip
// Write YAML to tmp file → loadFromConfig → generateYaml → validateConfig
// ─────────────────────────────────────────────────────────────────────────────

static std::string writeTmpYaml(const std::string& yaml)
{
    auto path = std::filesystem::temp_directory_path() / "wizard_test_roundtrip.yaml";
    std::ofstream f(path);
    f << yaml;
    return path.string();
}

TEST(WizardLoadFromConfig, RoundTripMldpPvxs)
{
    auto original = makeMinimalState();
    original.metrics_enabled  = true;
    original.metrics_endpoint = "0.0.0.0:9464";
    original.metrics_interval = "5";

    std::string yaml = generateYaml(original);
    std::string path = writeTmpYaml(yaml);

    WizardState loaded;
    loadFromConfig(path, loaded);

    EXPECT_EQ(original.controller_name, loaded.controller_name);
    ASSERT_EQ(1u, loaded.mldp_writers.size());
    EXPECT_EQ("mldp_main",                         loaded.mldp_writers[0].name);
    EXPECT_EQ("pvxs_provider",                      loaded.mldp_writers[0].provider_name);
    EXPECT_EQ("grpc://mldp-ingest.example.com:50051", loaded.mldp_writers[0].ingestion_url);
    ASSERT_EQ(1u, loaded.readers.size());
    EXPECT_EQ("pvxs_reader", loaded.readers[0].name);
    EXPECT_EQ("epics-pvxs",  loaded.readers[0].reader_type);
    EXPECT_TRUE(loaded.metrics_enabled);
    EXPECT_EQ("0.0.0.0:9464", loaded.metrics_endpoint);
    EXPECT_EQ("5",             loaded.metrics_interval);

    // Re-generated YAML must also pass validation
    auto regenYaml = generateYaml(loaded);
    auto diags     = validateYamlString(regenYaml);
    EXPECT_EQ(0, countErrors(diags)) << regenYaml;
}

TEST(WizardLoadFromConfig, RoundTripArchiverHistoricalOnce)
{
    auto st = makeMinimalState();

    EpicsReaderConfig arch;
    arch.name               = "archiver_hist";
    arch.reader_type        = "epics-archiver";
    arch.hostname           = "archiver.example.com:11200";
    arch.mode               = "historical_once";
    arch.start_date         = "2026-01-01T00:00:00Z";
    arch.end_date           = "2026-01-02T00:00:00Z";
    arch.connect_timeout_sec = "30";
    arch.total_timeout_sec   = "300";
    st.readers.push_back(arch);

    std::string yaml = generateYaml(st);
    std::string path = writeTmpYaml(yaml);

    WizardState loaded;
    loadFromConfig(path, loaded);

    ASSERT_EQ(2u, loaded.readers.size());
    const auto& la = loaded.readers[1];
    EXPECT_EQ("epics-archiver",        la.reader_type);
    EXPECT_EQ("historical_once",       la.mode);
    EXPECT_EQ("2026-01-01T00:00:00Z",  la.start_date);
    EXPECT_EQ("archiver.example.com:11200", la.hostname);

    auto regenYaml = generateYaml(loaded);
    auto diags     = validateYamlString(regenYaml);
    EXPECT_EQ(0, countErrors(diags)) << regenYaml;
}

TEST(WizardLoadFromConfig, RoundTripHdf5Writer)
{
    WizardState st;
    st.controller_name = "default";

    Hdf5WriterConfig hw;
    hw.name              = "hdf5_local";
    hw.base_path         = "/data/hdf5";
    hw.compression_level = "3";
    hw.max_file_age_s    = "7200";
    st.hdf5_writers.push_back(hw);

    EpicsReaderConfig r;
    r.name        = "pvxs_r";
    r.reader_type = "epics-pvxs";
    st.readers.push_back(r);

    std::string yaml = generateYaml(st);
    std::string path = writeTmpYaml(yaml);

    WizardState loaded;
    loadFromConfig(path, loaded);

    ASSERT_EQ(1u, loaded.hdf5_writers.size());
    EXPECT_EQ("hdf5_local",  loaded.hdf5_writers[0].name);
    EXPECT_EQ("/data/hdf5",  loaded.hdf5_writers[0].base_path);
    EXPECT_EQ("3",           loaded.hdf5_writers[0].compression_level);
    EXPECT_EQ("7200",        loaded.hdf5_writers[0].max_file_age_s);

    auto regenYaml = generateYaml(loaded);
    auto diags     = validateYamlString(regenYaml);
    EXPECT_EQ(0, countErrors(diags)) << regenYaml;
}

TEST(WizardLoadFromConfig, GracefulOnMissingFile)
{
    WizardState st;
    loadFromConfig("/nonexistent/path/to/config.yaml", st);
    // should not throw, state stays default
    EXPECT_TRUE(st.mldp_writers.empty());
    EXPECT_TRUE(st.readers.empty());
}

TEST(WizardLoadFromConfig, RoundTripPvsPreserved)
{
    auto st = makeMinimalState();

    PvEntry pv1; pv1.name = "SITE:A:PV"; pv1.option_type = "none";
    PvEntry pv2; pv2.name = "SITE:B:PV"; pv2.option_type = "scalar"; pv2.option_value = "chan://b";
    st.readers[0].pvs = {pv1, pv2};

    std::string yaml = generateYaml(st);
    std::string path = writeTmpYaml(yaml);

    WizardState loaded;
    loadFromConfig(path, loaded);

    ASSERT_EQ(1u, loaded.readers.size());
    ASSERT_EQ(2u, loaded.readers[0].pvs.size());
    EXPECT_EQ("SITE:A:PV", loaded.readers[0].pvs[0].name);
    EXPECT_EQ("SITE:B:PV", loaded.readers[0].pvs[1].name);
}

} // namespace mldp_pvxs_driver::config
