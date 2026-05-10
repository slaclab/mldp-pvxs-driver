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
      - name: pvxs_extra
        thread-pool: 1

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

TEST(ConfigRemove, RemoveLastReaderFails)
{
    auto path = writeTempFile(kMldpOnlyYaml);
    EditRemoveOptions opts;
    opts.path      = path;
    opts.kind      = "reader";
    opts.name      = "pvxs_main";
    opts.no_backup = true;

    int rc = 0;
    captureStderr([&]{ rc = runRemove(opts); });
    std::filesystem::remove(path);

    EXPECT_EQ(rc, 1);
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

} // namespace mldp_pvxs_driver::config
