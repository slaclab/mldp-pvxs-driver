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

#include <config/edit.h>
#include <config/wizard.h>
#include <config/validate.h>
#include "wizard_internal.h"
#include "test_config_helpers.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace mldp_pvxs_driver::config {

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture — writes temp YAML files and cleans up
// ─────────────────────────────────────────────────────────────────────────────

namespace {

const std::string kMldpOnlyYaml = R"yaml(
controller:
  name: test_ctrl

writer:
  mldp:
    - name: mldp_main
      thread-pool: 2
      stream-max-bytes: 2097152
      stream-max-age-ms: 200
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        min-conn: 1
        max-conn: 4
        credentials: ssl

reader:
  - epics-pvxs:
      - name: pvxs_main
        thread-pool: 4
        pvs:
          - name: SITE:SYS:PRESSURE
          - name: SITE:SYS:TEMPERATURE
)yaml";

const std::string kTwoWritersYaml = R"yaml(
controller:
  name: test_ctrl

writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      stream-max-bytes: 2097152
      stream-max-age-ms: 200
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        min-conn: 1
        max-conn: 4
        credentials: ssl
  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
      max-file-age-s: 3600
      max-file-size-mb: 512
      flush-interval-ms: 1000
      compression-level: 0

reader:
  - epics-pvxs:
      - name: pvxs_main
        thread-pool: 2
)yaml";

const std::string kWithRoutingYaml = R"yaml(
controller:
  name: test_ctrl

writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      stream-max-bytes: 2097152
      stream-max-age-ms: 200
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        min-conn: 1
        max-conn: 4
        credentials: ssl

reader:
  - epics-pvxs:
      - name: pvxs_main
        thread-pool: 2

routing:
  mldp_main:
    from:
      - pvxs_main
    include:
      - "SITE:BPM:*"
)yaml";

const std::string kWithMetricsYaml = R"yaml(
controller:
  name: test_ctrl

writer:
  mldp:
    - name: mldp_main
      thread-pool: 1
      stream-max-bytes: 2097152
      stream-max-age-ms: 200
      mldp-pool:
        provider-name: pvxs_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        min-conn: 1
        max-conn: 4
        credentials: ssl

reader:
  - epics-pvxs:
      - name: pvxs_main
        thread-pool: 2

metrics:
  endpoint: "0.0.0.0:9464"
  scan-interval-seconds: 5
)yaml";

// Write yaml to a temp file; caller is responsible for deletion
std::string writeTempFile(const std::string& yaml,
                          const std::string& suffix = ".yaml")
{
    auto tmp = std::filesystem::temp_directory_path() /
               ("edit_test_" + std::to_string(
                   std::hash<std::string>{}(yaml + suffix)) + suffix);
    std::ofstream out(tmp);
    out << yaml;
    return tmp.string();
}

// Capture stdout from a callable
template<typename Fn>
std::string captureStdout(Fn fn)
{
    std::ostringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    fn();
    std::cout.rdbuf(old);
    return buf.str();
}

// Capture stderr from a callable
template<typename Fn>
std::string captureStderr(Fn fn)
{
    std::ostringstream buf;
    std::streambuf* old = std::cerr.rdbuf(buf.rdbuf());
    fn();
    std::cerr.rdbuf(old);
    return buf.str();
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// config list tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConfigList, PrintsWriterNames)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditListOptions opts;
    opts.path = path;
    std::string out = captureStdout([&]{ runList(opts); });
    std::filesystem::remove(path);
    EXPECT_NE(std::string::npos, out.find("mldp_main"));
}

TEST(ConfigList, PrintsReaderNames)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditListOptions opts;
    opts.path = path;
    std::string out = captureStdout([&]{ runList(opts); });
    std::filesystem::remove(path);
    EXPECT_NE(std::string::npos, out.find("pvxs_main"));
    EXPECT_NE(std::string::npos, out.find("epics-pvxs"));
    EXPECT_NE(std::string::npos, out.find("PVs"));
}

TEST(ConfigList, PrintsRoutingAllToAll)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditListOptions opts;
    opts.path = path;
    std::string out = captureStdout([&]{ runList(opts); });
    std::filesystem::remove(path);
    EXPECT_NE(std::string::npos, out.find("all-to-all"));
}

TEST(ConfigList, PrintsExplicitRouting)
{
    auto path = writeTempFile(kWithRoutingYaml);
    EditListOptions opts;
    opts.path = path;
    std::string out = captureStdout([&]{ runList(opts); });
    std::filesystem::remove(path);
    EXPECT_NE(std::string::npos, out.find("mldp_main"));
    EXPECT_NE(std::string::npos, out.find("pvxs_main"));
}

TEST(ConfigList, PrintsMetrics)
{
    auto path = writeTempFile(kWithMetricsYaml);
    EditListOptions opts;
    opts.path = path;
    std::string out = captureStdout([&]{ runList(opts); });
    std::filesystem::remove(path);
    EXPECT_NE(std::string::npos, out.find("0.0.0.0:9464"));
}

TEST(ConfigList, MetricsDisabled)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditListOptions opts;
    opts.path = path;
    std::string out = captureStdout([&]{ runList(opts); });
    std::filesystem::remove(path);
    EXPECT_NE(std::string::npos, out.find("disabled"));
}

TEST(ConfigList, InvalidFileExits1)
{
    EditListOptions opts;
    opts.path = "/nonexistent/path/config.yaml";
    std::string err = captureStderr([&]{
        int rc = runList(opts);
        EXPECT_EQ(1, rc);
    });
    EXPECT_NE(std::string::npos, err.find("ERROR"));
}

// ─────────────────────────────────────────────────────────────────────────────
// config remove tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConfigRemove, RemoveWriterByName)
{
    auto path = writeTempFile(kTwoWritersYaml);
    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "hdf5_local";
    opts.no_backup = true;

    int rc = runRemove(opts);
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    EXPECT_TRUE(st.hdf5_writers.empty());
    EXPECT_EQ(1u, st.mldp_writers.size());
}

TEST(ConfigRemove, RemoveWriterCleansRouting)
{
    auto path = writeTempFile(kWithRoutingYaml);
    // Add hdf5 writer so removing mldp_main is not the last writer
    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    Hdf5WriterConfig h;
    h.name      = "hdf5_extra";
    h.base_path = "/tmp/hdf5";
    st.hdf5_writers.push_back(h);
    {
        std::ofstream out(path);
        out << wizard_internal::generateYaml(st);
    }

    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "mldp_main";
    opts.no_backup = true;

    int rc = runRemove(opts);
    EXPECT_EQ(0, rc);

    WizardState st2;
    wizard_internal::loadFromConfig(path, st2);
    std::filesystem::remove(path);

    EXPECT_TRUE(st2.mldp_writers.empty());
    for (const auto& re : st2.routing)
        EXPECT_NE("mldp_main", re.writer_name);
}

TEST(ConfigRemove, RemoveReaderByName)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "reader";
    opts.name      = "pvxs_main";
    opts.no_backup = true;

    // Remove should succeed (config may warn but not fail on missing reader)
    captureStderr([&]{
        int rc = runRemove(opts);
        // validateConfig may emit warnings for no reader, but we only block on errors
        (void)rc;
    });

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    EXPECT_TRUE(st.readers.empty());
}

TEST(ConfigRemove, RemoveReaderCleansFromLists)
{
    auto path = writeTempFile(kWithRoutingYaml);
    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "reader";
    opts.name      = "pvxs_main";
    opts.no_backup = true;

    captureStderr([&]{ runRemove(opts); });

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    for (const auto& re : st.routing)
        for (const auto& r : re.from_readers)
            EXPECT_NE("pvxs_main", r);
}

TEST(ConfigRemove, RemoveRoutingByName)
{
    auto path = writeTempFile(kWithRoutingYaml);
    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "routing";
    opts.name      = "mldp_main";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runRemove(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    for (const auto& re : st.routing)
        EXPECT_NE("mldp_main", re.writer_name);
}

TEST(ConfigRemove, RemoveLastWriterFails)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    auto orig_mtime = std::filesystem::last_write_time(path);

    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "mldp_main";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runRemove(opts); });
    EXPECT_EQ(1, rc);
    EXPECT_EQ(orig_mtime, std::filesystem::last_write_time(path));
    std::filesystem::remove(path);
}

TEST(ConfigRemove, RemoveUnknownNameFails)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    auto orig_mtime = std::filesystem::last_write_time(path);

    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "nonexistent_writer";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runRemove(opts); });
    EXPECT_EQ(1, rc);
    EXPECT_EQ(orig_mtime, std::filesystem::last_write_time(path));
    std::filesystem::remove(path);
}

TEST(ConfigRemove, RemoveDryRun)
{
    auto path = writeTempFile(kTwoWritersYaml);
    auto orig_content = [&]{
        std::ifstream f(path);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }();

    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "hdf5_local";
    opts.no_backup = true;
    opts.dry_run   = true;

    std::string out = captureStdout([&]{
        captureStderr([&]{ runRemove(opts); });
    });

    // File unchanged
    std::ifstream f(path);
    std::string after(std::istreambuf_iterator<char>(f), {});
    std::filesystem::remove(path);

    EXPECT_EQ(orig_content, after);
    EXPECT_NE(std::string::npos, out.find("writer:"));
}

TEST(ConfigRemove, RemoveWritesBackup)
{
    auto path = writeTempFile(kTwoWritersYaml);
    std::string bak = path + ".bak";
    std::filesystem::remove(bak);

    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "hdf5_local";
    opts.no_backup = false;

    captureStderr([&]{ runRemove(opts); });

    bool bak_exists = std::filesystem::exists(bak);
    std::filesystem::remove(path);
    std::filesystem::remove(bak);

    EXPECT_TRUE(bak_exists);
}

TEST(ConfigRemove, RemoveNoBackup)
{
    auto path = writeTempFile(kTwoWritersYaml);
    std::string bak = path + ".bak";
    std::filesystem::remove(bak);

    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "hdf5_local";
    opts.no_backup = true;

    captureStderr([&]{ runRemove(opts); });

    bool bak_exists = std::filesystem::exists(bak);
    std::filesystem::remove(path);

    EXPECT_FALSE(bak_exists);
}

// ─────────────────────────────────────────────────────────────────────────────
// config add non-interactive tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConfigAdd, AddMldpWriterNonInteractive)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path          = path;
    opts.kind          = "writer";
    opts.name          = "mldp_extra";
    opts.type          = "mldp";
    opts.ingestion_url = "grpc://mldp2.example.com:50051";
    opts.provider_name = "pvxs_provider2";
    opts.no_backup     = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    bool found = false;
    for (const auto& w : st.mldp_writers)
        if (w.name == "mldp_extra") { found = true; break; }
    EXPECT_TRUE(found);
}

TEST(ConfigAdd, AddHdf5WriterNonInteractive)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "hdf5_new";
    opts.type      = "hdf5";
    opts.base_path = "/data/hdf5";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    bool found = false;
    for (const auto& w : st.hdf5_writers)
        if (w.name == "hdf5_new" && w.base_path == "/data/hdf5") { found = true; break; }
    EXPECT_TRUE(found);
}

TEST(ConfigAdd, AddEpicsPvxsReaderNonInteractive)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path      = path;
    opts.kind      = "reader";
    opts.name      = "pvxs_extra";
    opts.type      = "epics-pvxs";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    bool found = false;
    for (const auto& r : st.readers)
        if (r.name == "pvxs_extra") { found = true; break; }
    EXPECT_TRUE(found);
}

TEST(ConfigAdd, AddReaderWithPvList)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path      = path;
    opts.kind      = "reader";
    opts.name      = "pvxs_pv_test";
    opts.type      = "epics-pvxs";
    opts.pvs       = "PV:A,PV:B,PV:C";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    for (const auto& r : st.readers) {
        if (r.name == "pvxs_pv_test") {
            EXPECT_EQ(3u, r.pvs.size());
            return;
        }
    }
    FAIL() << "reader pvxs_pv_test not found";
}

TEST(ConfigAdd, AddArchiverHistoricalOnce)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path       = path;
    opts.kind       = "reader";
    opts.name       = "arch_hist";
    opts.type       = "epics-archiver";
    opts.hostname   = "archiver.example.com:17668";
    opts.mode       = "historical_once";
    opts.start_date = "2026-01-01";
    opts.no_backup  = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    bool found = false;
    for (const auto& r : st.readers)
        if (r.name == "arch_hist" && r.mode == "historical_once") { found = true; break; }
    EXPECT_TRUE(found);
}

TEST(ConfigAdd, AddArchiverPeriodicTail)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path             = path;
    opts.kind             = "reader";
    opts.name             = "arch_tail";
    opts.type             = "epics-archiver";
    opts.hostname         = "archiver.example.com:17668";
    opts.mode             = "periodic_tail";
    opts.poll_interval_sec = "10";
    opts.no_backup        = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(0, rc);

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    bool found = false;
    for (const auto& r : st.readers)
        if (r.name == "arch_tail" && r.mode == "periodic_tail") { found = true; break; }
    EXPECT_TRUE(found);
}

TEST(ConfigAdd, AddRoutingNonInteractive)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditAddOptions opts;
    opts.path         = path;
    opts.kind         = "routing";
    opts.writer_name  = "mldp_main";
    opts.from         = "pvxs_main";
    opts.include_globs = {"SITE:BPM:*"};
    opts.no_backup    = true;

    std::string err = captureStderr([&]{ runAdd(opts); });

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    ASSERT_FALSE(st.routing.empty());
    EXPECT_EQ("mldp_main", st.routing[0].writer_name);
    EXPECT_EQ(1u, st.routing[0].from_readers.size());
    EXPECT_EQ("pvxs_main", st.routing[0].from_readers[0]);
    EXPECT_EQ(1u, st.routing[0].include_globs.size());
}

TEST(ConfigAdd, AddRoutingMerge)
{
    auto path = writeTempFile(kWithRoutingYaml);
    EditAddOptions opts;
    opts.path        = path;
    opts.kind        = "routing";
    opts.writer_name = "mldp_main";
    opts.from        = "pvxs_extra";
    opts.no_backup   = true;

    captureStderr([&]{ runAdd(opts); });

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    ASSERT_FALSE(st.routing.empty());
    bool has_pvxs_extra = false;
    for (const auto& r : st.routing[0].from_readers)
        if (r == "pvxs_extra") { has_pvxs_extra = true; break; }
    EXPECT_TRUE(has_pvxs_extra);
}

TEST(ConfigAdd, AddRoutingReplace)
{
    auto path = writeTempFile(kWithRoutingYaml);
    EditAddOptions opts;
    opts.path        = path;
    opts.kind        = "routing";
    opts.writer_name = "mldp_main";
    opts.from        = "pvxs_extra";
    opts.replace     = true;
    opts.no_backup   = true;

    captureStderr([&]{ runAdd(opts); });

    WizardState st;
    wizard_internal::loadFromConfig(path, st);
    std::filesystem::remove(path);

    ASSERT_FALSE(st.routing.empty());
    EXPECT_EQ(1u, st.routing[0].from_readers.size());
    EXPECT_EQ("pvxs_extra", st.routing[0].from_readers[0]);
    EXPECT_TRUE(st.routing[0].include_globs.empty());
}

TEST(ConfigAdd, AddDuplicateNameFails)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    auto orig_mtime = std::filesystem::last_write_time(path);

    EditAddOptions opts;
    opts.path          = path;
    opts.kind          = "writer";
    opts.name          = "mldp_main";
    opts.type          = "mldp";
    opts.ingestion_url = "grpc://other.example.com:50051";
    opts.provider_name = "other_provider";
    opts.no_backup     = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(1, rc);
    EXPECT_EQ(orig_mtime, std::filesystem::last_write_time(path));
    std::filesystem::remove(path);
}

TEST(ConfigAdd, AddMissingRequiredFlagFails)
{
    auto path = writeTempFile(kMldpOnlyYaml);

    EditAddOptions opts;
    opts.path      = path;
    opts.kind      = "writer";
    opts.name      = "mldp_new";
    opts.type      = "mldp";
    // Missing ingestion_url and provider_name
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runAdd(opts); });
    EXPECT_EQ(1, rc);
    std::filesystem::remove(path);
}

TEST(ConfigAdd, AddDryRun)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    auto orig_content = [&]{
        std::ifstream f(path);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }();

    EditAddOptions opts;
    opts.path          = path;
    opts.kind          = "writer";
    opts.name          = "mldp_dry";
    opts.type          = "mldp";
    opts.ingestion_url = "grpc://dry.example.com:50051";
    opts.provider_name = "dry_provider";
    opts.no_backup     = true;
    opts.dry_run       = true;

    std::string out = captureStdout([&]{
        captureStderr([&]{ runAdd(opts); });
    });

    std::ifstream f(path);
    std::string after(std::istreambuf_iterator<char>(f), {});
    std::filesystem::remove(path);

    EXPECT_EQ(orig_content, after);
    EXPECT_NE(std::string::npos, out.find("mldp_dry"));
}

} // namespace mldp_pvxs_driver::config
