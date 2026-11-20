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

#include <cli/FileLogging.h>
#include <cli/LoggingConfig.h>
#include <cli/RotatingGzipFileSink.h>

#include <util/log/Logger.h>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <thread>
#include <sstream>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../config/test_config_helpers.h"

using namespace mldp_pvxs_driver::cli;
using mldp_pvxs_driver::config::makeConfigFromYaml;
namespace fs = std::filesystem;

namespace {

class TempDir
{
public:
    TempDir()
    {
        path_ = fs::temp_directory_path() /
                ("mldp_log_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::vector<std::string> listNames(const fs::path& dir)
{
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir))
        out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

int countSuffix(const std::vector<std::string>& names, const std::string& suffix)
{
    int n = 0;
    for (const auto& s : names)
        if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            ++n;
    return n;
}

std::string gunzip(const fs::path& gz)
{
    gzFile f = gzopen(gz.c_str(), "rb");
    EXPECT_NE(f, nullptr);
    std::string out;
    char        buf[4096];
    int         n;
    while ((n = gzread(f, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    gzclose(f);
    return out;
}

std::shared_ptr<spdlog::logger> makeLogger(std::shared_ptr<RotatingGzipFileSink> sink)
{
    sink->set_pattern("%v");
    return std::make_shared<spdlog::logger>("t", sink);
}

} // namespace

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, ClassifyKnownLoggerNames)
{
    auto r = classifyLogger("reader:slac-calendar:slac_calendar_local");
    EXPECT_EQ(r.category, LogCategory::Reader);
    EXPECT_EQ(r.fileStem, "reader-slac-calendar_slac_calendar_local");

    r = classifyLogger("configuration_writer:mldp_config_main");
    EXPECT_EQ(r.category, LogCategory::Writer);
    EXPECT_EQ(r.fileStem, "writer-configuration_mldp_config_main");

    r = classifyLogger("controller.default");
    EXPECT_EQ(r.category, LogCategory::Controller);
    EXPECT_EQ(r.fileStem, "controller-default");

    r = classifyLogger("epics_pvxs_reader");
    EXPECT_EQ(r.category, LogCategory::Reader);
    EXPECT_EQ(r.fileStem, "reader-epics_pvxs_reader");

    EXPECT_EQ(classifyLogger("util:http:curl").category, LogCategory::Other);
    EXPECT_EQ(classifyLogger("mldp_grpc_pool").category, LogCategory::Other);
    EXPECT_EQ(classifyLogger("processor.x").category, LogCategory::Other);
}

TEST(FileLoggingTest, SanitizeFileName)
{
    EXPECT_EQ(sanitizeLogFileName("a b/c:d.e-f_g"), "a_b_c_d.e-f_g");
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, ConfigDefaultsWhenBlockMissing)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml("controller:\n  name: x\n"));
    EXPECT_FALSE(cfg.fileLoggingEnabled());
    EXPECT_TRUE(cfg.console);
    EXPECT_TRUE(cfg.logFile.empty());
    EXPECT_FALSE(cfg.anyPerComponent());
    EXPECT_EQ(cfg.rotation.maxBytes, 100ULL * 1024 * 1024);
    EXPECT_EQ(cfg.rotation.maxFiles, 10);
}

TEST(FileLoggingTest, ConfigParsesBlock)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml(
        "logging:\n"
        "  log-file: /tmp/x/all.log\n"
        "  console: false\n"
        "  per-run: false\n"
        "  per-component:\n"
        "    readers: true\n"
        "  rotation:\n"
        "    max-size-mb: 5\n"
        "    max-age-hours: 2\n"
        "    compress: false\n"
        "    max-files: 3\n"));
    EXPECT_TRUE(cfg.fileLoggingEnabled());
    EXPECT_FALSE(cfg.console);
    EXPECT_EQ(cfg.logFileDirectory(), "/tmp/x");
    EXPECT_EQ(cfg.logFileStem(), "all");
    EXPECT_FALSE(cfg.perRun);
    EXPECT_TRUE(cfg.perReaders);
    EXPECT_FALSE(cfg.perWriters);
    EXPECT_EQ(cfg.rotation.maxBytes, 5ULL * 1024 * 1024);
    EXPECT_EQ(cfg.rotation.maxAge, std::chrono::hours(2));
    EXPECT_FALSE(cfg.rotation.compress);
    EXPECT_EQ(cfg.rotation.maxFiles, 3);
}

TEST(FileLoggingTest, ConfigLogFileOnlyMeansNoComponentFiles)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml("logging:\n  log-file: driver.log\n"));
    EXPECT_TRUE(cfg.fileLoggingEnabled());
    EXPECT_FALSE(cfg.anyPerComponent());
    EXPECT_EQ(cfg.logFileDirectory(), ".");
    EXPECT_EQ(cfg.logFileStem(), "driver");
}

TEST(FileLoggingTest, ConfigComponentFilesNeedLogFile)
{
    EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml(
                     "logging:\n  per-component:\n    readers: true\n")),
                 std::runtime_error);
    // CLI --log-file satisfies the requirement.
    const auto cfg = LoggingConfig::parse(
        makeConfigFromYaml("logging:\n  per-component:\n    readers: true\n"), "logs/driver.log");
    EXPECT_TRUE(cfg.fileLoggingEnabled());
    EXPECT_EQ(cfg.logFileDirectory(), "logs");
}

TEST(FileLoggingTest, ConfigRejectsDirectoryKey)
{
    EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml(
                     "logging:\n  log-file: logs/driver.log\n  directory: other\n")),
                 std::runtime_error);
}

TEST(FileLoggingTest, ConfigRejectsOldCombinedFileKey)
{
    EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml(
                     "logging:\n  combined-file: driver\n")),
                 std::runtime_error);
}

TEST(FileLoggingTest, ConfigRejectsNegative)
{
    EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml(
                     "logging:\n  log-file: d.log\n  rotation:\n    max-files: -1\n")),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// Sink rotation / compression / retention
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, SizeRotationCompressesRotatedFile)
{
    TempDir dir;
    auto    compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 100;
    policy.maxFiles = 0;
    auto sink   = std::make_shared<RotatingGzipFileSink>(dir.path(), "comp", "", policy, compressor);
    auto logger = makeLogger(sink);

    const std::string line(60, 'a');
    logger->info(line); // 61 bytes
    logger->info(line); // would exceed 100 -> rotate first
    logger->flush();
    compressor->waitIdle();

    const auto names = listNames(dir.path());
    EXPECT_EQ(countSuffix(names, ".log.gz"), 1) << ::testing::PrintToString(names);
    EXPECT_TRUE(fs::exists(dir.path() / "comp.log"));

    for (const auto& n : names)
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".gz") == 0)
            EXPECT_EQ(gunzip(dir.path() / n), line + "\n");
}

TEST(FileLoggingTest, AgeRotationUsesClock)
{
    TempDir dir;
    auto    compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 0;
    policy.maxAge   = std::chrono::hours(1);
    policy.compress = false;
    policy.maxFiles = 0;

    auto now  = std::chrono::system_clock::now();
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "age", "", policy, compressor,
                                                       [&now] { return now; });
    auto logger = makeLogger(sink);
    logger->info("first");
    now += std::chrono::hours(2);
    logger->info("second");
    logger->flush();
    compressor->waitIdle();

    const auto names = listNames(dir.path());
    EXPECT_EQ(names.size(), 2u) << ::testing::PrintToString(names);
    EXPECT_EQ(countSuffix(names, ".gz"), 0);
}

TEST(FileLoggingTest, RetentionKeepsNewestFiles)
{
    TempDir dir;
    auto    compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 10;
    policy.maxFiles = 2;

    auto now  = std::chrono::system_clock::now();
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "keep", "", policy, compressor,
                                                       [&now] { return now; });
    auto logger = makeLogger(sink);
    for (int i = 0; i < 6; ++i)
    {
        logger->info("line-{:04d}", i); // each line > 10 bytes -> rotate every write
        now += std::chrono::seconds(1);
        compressor->waitIdle();
    }
    logger->flush();
    compressor->waitIdle();

    const auto names = listNames(dir.path());
    EXPECT_TRUE(fs::exists(dir.path() / "keep.log"));
    EXPECT_EQ(countSuffix(names, ".log.gz"), 2) << ::testing::PrintToString(names);
}

TEST(FileLoggingTest, PerRunNameAndPreviousRunCompressed)
{
    TempDir dir;
    {
        std::ofstream old(dir.path() / "run.20200101-000000.log");
        old << "old run\n";
    }
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "run", "20261118-120000", policy,
                                                       compressor);
    EXPECT_EQ(sink->activePath().filename().string(), "run.20261118-120000.log");
    compressor->waitIdle();

    EXPECT_TRUE(fs::exists(dir.path() / "run.20200101-000000.log.gz"));
    EXPECT_FALSE(fs::exists(dir.path() / "run.20200101-000000.log"));
    EXPECT_EQ(gunzip(dir.path() / "run.20200101-000000.log.gz"), "old run\n");
}

// ===========================================================================
// Additional coverage
// ===========================================================================

namespace {

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Files in @p dir whose name starts with "<stem>." and ends with ".log".
std::vector<fs::path> activeLogs(const fs::path& dir, const std::string& stem)
{
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const auto n = e.path().filename().string();
        if (n.rfind(stem + ".", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
            out.push_back(e.path());
    }
    return out;
}

void touch(const fs::path& p, const std::string& content,
           fs::file_time_type when = fs::file_time_type::clock::now())
{
    std::ofstream(p) << content;
    fs::last_write_time(p, when);
}

} // namespace

// ---------------------------------------------------------------------------
// Routing — remaining name shapes
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, ClassifyAllWriterAndReaderShapes)
{
    EXPECT_EQ(classifyLogger("grpc_writer:w1").fileStem, "writer-grpc_w1");
    EXPECT_EQ(classifyLogger("hdf5_writer:h5").fileStem, "writer-hdf5_h5");
    EXPECT_EQ(classifyLogger("pv_metadata_writer:meta").fileStem, "writer-pv_metadata_meta");
    EXPECT_EQ(classifyLogger("reader:epics-archiver:arch 1").fileStem, "reader-epics-archiver_arch_1");
    EXPECT_EQ(classifyLogger("reader:epics-ds-metadata:ds").fileStem, "reader-epics-ds-metadata_ds");
    EXPECT_EQ(classifyLogger("hdf5_bsas_gen1_reader").fileStem, "reader-hdf5_bsas_gen1_reader");
    EXPECT_EQ(classifyLogger("epics_base_reader").category, LogCategory::Reader);

    for (const char* other : {"", "mldp_pvxs_driver", "mldp_grpc_annotation_pool",
                              "mldp_grpc_query_pool", "util:http:curl", "controller"})
    {
        const auto r = classifyLogger(other);
        EXPECT_EQ(r.category, LogCategory::Other) << other;
        EXPECT_TRUE(r.fileStem.empty()) << other;
    }
}

TEST(FileLoggingTest, SanitizeKeepsAllowedAndEmpty)
{
    EXPECT_EQ(sanitizeLogFileName(""), "");
    EXPECT_EQ(sanitizeLogFileName("AZaz09._-"), "AZaz09._-");
    EXPECT_EQ(sanitizeLogFileName("é/\\*?"), std::string(sanitizeLogFileName("é").size(), '_') + "____");
}

// ---------------------------------------------------------------------------
// Config — remaining rules
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, ConfigBlockWithoutLogFileIsConsoleOnly)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml("logging:\n  console: false\n"));
    EXPECT_FALSE(cfg.fileLoggingEnabled());
    EXPECT_FALSE(cfg.console);
}

TEST(FileLoggingTest, ConfigEachPerComponentFlag)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml(
        "logging:\n  log-file: l/d.log\n  per-component:\n    writers: true\n    controller: true\n"));
    EXPECT_FALSE(cfg.perReaders);
    EXPECT_TRUE(cfg.perWriters);
    EXPECT_TRUE(cfg.perController);
    EXPECT_TRUE(cfg.anyPerComponent());

    for (const char* key : {"readers", "writers", "controller"})
        EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml(
                         std::string("logging:\n  per-component:\n    ") + key + ": true\n")),
                     std::runtime_error)
            << key;
}

TEST(FileLoggingTest, ConfigLogFileOverrideReplacesYaml)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml("logging:\n  log-file: a/x.log\n"),
                                          "b/y.log");
    EXPECT_EQ(cfg.logFile, "b/y.log");
    EXPECT_EQ(cfg.logFileDirectory(), "b");
    EXPECT_EQ(cfg.logFileStem(), "y");

    // Override alone (no logging block) enables file logging.
    const auto cli = LoggingConfig::parse(makeConfigFromYaml("controller:\n  name: x\n"), "z.log");
    EXPECT_TRUE(cli.fileLoggingEnabled());
}

TEST(FileLoggingTest, ConfigLogFileStemVariants)
{
    LoggingConfig cfg;
    cfg.logFile = "logs/driver";
    EXPECT_EQ(cfg.logFileStem(), "driver");
    cfg.logFile = "logs/run.calendar.log";
    EXPECT_EQ(cfg.logFileStem(), "run.calendar");
    cfg.logFile = "logs/driver.txt";
    EXPECT_EQ(cfg.logFileStem(), "driver.txt");
    cfg.logFile = ".log";
    EXPECT_EQ(cfg.logFileStem(), ".log");
    cfg.logFile = "/abs/dir/d.log";
    EXPECT_EQ(cfg.logFileDirectory(), "/abs/dir");
}

TEST(FileLoggingTest, ConfigRejectsFolderAsLogFile)
{
    EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml("logging:\n  log-file: logs/\n")),
                 std::runtime_error);
}

TEST(FileLoggingTest, ConfigRejectsEachNegativeRotationValue)
{
    for (const char* key : {"max-size-mb", "max-age-hours", "max-files"})
        EXPECT_THROW(LoggingConfig::parse(makeConfigFromYaml(
                         std::string("logging:\n  log-file: d.log\n  rotation:\n    ") + key + ": -1\n")),
                     std::runtime_error)
            << key;
}

TEST(FileLoggingTest, ConfigRotationZeroValuesAllowed)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml(
        "logging:\n  log-file: d.log\n  rotation:\n    max-size-mb: 0\n    max-age-hours: 0\n"
        "    max-files: 0\n"));
    EXPECT_EQ(cfg.rotation.maxBytes, 0u);
    EXPECT_EQ(cfg.rotation.maxAge.count(), 0);
    EXPECT_EQ(cfg.rotation.maxFiles, 0);
}

// ---------------------------------------------------------------------------
// Sink — remaining behaviors
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, SizeRotationDisabledWhenZero)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 0;
    auto sink   = std::make_shared<RotatingGzipFileSink>(dir.path(), "nosz", "", policy, compressor);
    auto logger = makeLogger(sink);
    for (int i = 0; i < 200; ++i)
        logger->info(std::string(100, 'x'));
    logger->flush();
    compressor->waitIdle();
    EXPECT_EQ(listNames(dir.path()), std::vector<std::string>{"nosz.log"});
    EXPECT_EQ(fs::file_size(dir.path() / "nosz.log"), 200u * 101u);
}

TEST(FileLoggingTest, OversizedLineOnEmptyFileIsWrittenWithoutRotation)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 10;
    auto sink   = std::make_shared<RotatingGzipFileSink>(dir.path(), "big", "", policy, compressor);
    auto logger = makeLogger(sink);
    logger->info(std::string(50, 'y'));
    logger->flush();
    compressor->waitIdle();
    EXPECT_EQ(listNames(dir.path()), std::vector<std::string>{"big.log"});
    EXPECT_EQ(readFile(dir.path() / "big.log"), std::string(50, 'y') + "\n");
}

TEST(FileLoggingTest, RotationNameCollisionAddsCounter)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 5;
    policy.compress = false;
    policy.maxFiles = 0;
    const auto fixed = std::chrono::system_clock::now();
    auto       sink  = std::make_shared<RotatingGzipFileSink>(dir.path(), "col", "", policy, compressor,
                                                       [fixed] { return fixed; });
    auto logger = makeLogger(sink);
    for (int i = 0; i < 4; ++i)
        logger->info("line{}", i); // every write after the first rotates, same timestamp
    logger->flush();
    compressor->waitIdle();

    const std::string ts = formatLogTimestamp(fixed);
    EXPECT_TRUE(fs::exists(dir.path() / ("col." + ts + ".log")));
    EXPECT_TRUE(fs::exists(dir.path() / ("col." + ts + ".1.log")));
    EXPECT_TRUE(fs::exists(dir.path() / ("col." + ts + ".2.log")));
    EXPECT_EQ(readFile(dir.path() / "col.log"), "line3\n");
    EXPECT_EQ(readFile(dir.path() / ("col." + ts + ".log")), "line0\n");
}

TEST(FileLoggingTest, RotatedNameKeepsRunTag)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 5;
    policy.compress = false;
    const auto fixed = std::chrono::system_clock::now();
    auto       sink  = std::make_shared<RotatingGzipFileSink>(dir.path(), "rt", "RUN", policy, compressor,
                                                       [fixed] { return fixed; });
    auto logger = makeLogger(sink);
    logger->info("aaaaaa");
    logger->info("bbbbbb");
    logger->flush();
    compressor->waitIdle();
    EXPECT_TRUE(fs::exists(dir.path() / "rt.RUN.log"));
    EXPECT_TRUE(fs::exists(dir.path() / ("rt.RUN." + formatLogTimestamp(fixed) + ".log")));
}

TEST(FileLoggingTest, AppendModeKeepsExistingContentAndCountsItsSize)
{
    TempDir dir;
    touch(dir.path() / "app.log", std::string(90, 'o') + "\n");
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 100;
    policy.compress = false;
    auto sink   = std::make_shared<RotatingGzipFileSink>(dir.path(), "app", "", policy, compressor);
    auto logger = makeLogger(sink);
    logger->info("0123456789abc"); // 91 + 14 > 100 -> rotates the pre-existing content away
    logger->flush();
    compressor->waitIdle();

    EXPECT_EQ(readFile(dir.path() / "app.log"), "0123456789abc\n");
    EXPECT_EQ(countSuffix(listNames(dir.path()), ".log"), 2);
}

TEST(FileLoggingTest, NoCompressionKeepsPlainRotatedFiles)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 5;
    policy.compress = false;
    policy.maxFiles = 0;
    auto now  = std::chrono::system_clock::now();
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "plain", "", policy, compressor,
                                                       [&now] { return now; });
    auto logger = makeLogger(sink);
    for (int i = 0; i < 3; ++i)
    {
        logger->info("xxxxxx");
        now += std::chrono::seconds(1);
    }
    logger->flush();
    compressor->waitIdle();
    const auto names = listNames(dir.path());
    EXPECT_EQ(countSuffix(names, ".gz"), 0);
    EXPECT_EQ(countSuffix(names, ".log"), 3);
}

TEST(FileLoggingTest, AgeRotationNotTriggeredBeforeLimit)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 0;
    policy.maxAge   = std::chrono::hours(1);
    auto now  = std::chrono::system_clock::now();
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "young", "", policy, compressor,
                                                       [&now] { return now; });
    auto logger = makeLogger(sink);
    logger->info("a");
    now += std::chrono::minutes(59);
    logger->info("b");
    logger->flush();
    compressor->waitIdle();
    EXPECT_EQ(listNames(dir.path()), std::vector<std::string>{"young.log"});

    now += std::chrono::minutes(1); // exactly the limit -> rotates
    logger->info("c");
    logger->flush();
    compressor->waitIdle();
    EXPECT_EQ(listNames(dir.path()).size(), 2u);
}

TEST(FileLoggingTest, SinkCreatesMissingDirectory)
{
    TempDir           dir;
    const fs::path    nested = dir.path() / "a" / "b";
    auto              compressor = std::make_shared<LogCompressor>();
    auto sink = std::make_shared<RotatingGzipFileSink>(nested, "n", "", LogRotationPolicy{}, compressor);
    EXPECT_TRUE(fs::exists(nested / "n.log"));
}

TEST(FileLoggingTest, SinkWithoutCompressorStillRotates)
{
    TempDir           dir;
    LogRotationPolicy policy;
    policy.maxBytes = 5;
    auto sink   = std::make_shared<RotatingGzipFileSink>(dir.path(), "nc", "", policy, nullptr);
    auto logger = makeLogger(sink);
    logger->info("aaaaaa");
    logger->info("bbbbbb");
    logger->flush();
    EXPECT_EQ(countSuffix(listNames(dir.path()), ".log"), 2);
}

TEST(FileLoggingTest, LeftoversNotCompressedWhenCompressionOffButPruned)
{
    TempDir    dir;
    const auto base = fs::file_time_type::clock::now() - std::chrono::hours(10);
    for (int i = 0; i < 4; ++i)
        touch(dir.path() / ("lo.2020010" + std::to_string(i) + "-000000.log"), "x",
              base + std::chrono::hours(i));
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.compress = false;
    policy.maxFiles = 2;
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "lo", "NOW", policy, compressor);
    compressor->waitIdle();

    const auto names = listNames(dir.path());
    EXPECT_EQ(countSuffix(names, ".gz"), 0);
    // active + the 2 newest leftovers
    EXPECT_EQ(names, (std::vector<std::string>{"lo.20200102-000000.log", "lo.20200103-000000.log",
                                               "lo.NOW.log"}));
}

TEST(FileLoggingTest, LeftoverScanIgnoresOtherStemsAndActiveFile)
{
    TempDir dir;
    touch(dir.path() / "fam.OLD.log", "old\n");
    touch(dir.path() / "family.OLD.log", "other stem\n");
    touch(dir.path() / "fam.NOW.log", "existing active\n");
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    auto sink = std::make_shared<RotatingGzipFileSink>(dir.path(), "fam", "NOW", policy, compressor);
    compressor->waitIdle();

    EXPECT_TRUE(fs::exists(dir.path() / "fam.OLD.log.gz"));
    EXPECT_TRUE(fs::exists(dir.path() / "family.OLD.log"));
    EXPECT_EQ(readFile(dir.path() / "fam.NOW.log"), "existing active\n");
}

// ---------------------------------------------------------------------------
// LogCompressor — direct
// ---------------------------------------------------------------------------

TEST(FileLoggingTest, PruneUnlimitedWhenZero)
{
    TempDir dir;
    for (int i = 0; i < 5; ++i)
        touch(dir.path() / ("p." + std::to_string(i) + ".log"), "x");
    LogCompressor::prune(dir.path(), "p", "p.log", 0);
    EXPECT_EQ(listNames(dir.path()).size(), 5u);
}

TEST(FileLoggingTest, PruneOnlyTouchesFamilyLogFiles)
{
    TempDir    dir;
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(5);
    touch(dir.path() / "p.1.log", "x", old);
    touch(dir.path() / "p.2.log.gz", "x", old + std::chrono::hours(1));
    touch(dir.path() / "p.3.log", "x", old + std::chrono::hours(2));
    touch(dir.path() / "p.4.log.gz.tmp", "x", old - std::chrono::hours(1)); // in-progress gzip
    touch(dir.path() / "p.5.txt", "x", old - std::chrono::hours(1));        // not a log
    touch(dir.path() / "px.1.log", "x", old - std::chrono::hours(1));       // other stem
    touch(dir.path() / "p.log", "x", old - std::chrono::hours(2));          // active
    LogCompressor::prune(dir.path(), "p", "p.log", 1);

    EXPECT_EQ(listNames(dir.path()),
              (std::vector<std::string>{"p.3.log", "p.4.log.gz.tmp", "p.5.txt", "p.log", "px.1.log"}));
}

TEST(FileLoggingTest, PruneMissingDirectoryIsHarmless)
{
    EXPECT_NO_THROW(LogCompressor::prune("/nonexistent/mldp/logs", "p", "p.log", 1));
}

TEST(FileLoggingTest, GzipFileErrors)
{
    TempDir dir;
    EXPECT_FALSE(LogCompressor::gzipFile(dir.path() / "missing.log", dir.path() / "missing.log.gz"));
    touch(dir.path() / "src.log", "data\n");
    EXPECT_FALSE(LogCompressor::gzipFile(dir.path() / "src.log", dir.path() / "no" / "such" / "x.gz"));
    EXPECT_TRUE(fs::exists(dir.path() / "src.log"));
}

TEST(FileLoggingTest, GzipFileRoundTripLargeContent)
{
    TempDir     dir;
    std::string content;
    for (int i = 0; i < 20000; ++i)
        content += "line " + std::to_string(i) + "\n"; // > 64 KiB buffer
    touch(dir.path() / "large.log", content);
    ASSERT_TRUE(LogCompressor::gzipFile(dir.path() / "large.log", dir.path() / "large.log.gz"));
    EXPECT_EQ(gunzip(dir.path() / "large.log.gz"), content);
    EXPECT_FALSE(fs::exists(dir.path() / "large.log.gz.tmp"));
}

TEST(FileLoggingTest, CompressorSkipsMissingFileAndStillPrunes)
{
    TempDir    dir;
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(3);
    touch(dir.path() / "m.1.log.gz", "x", old);
    touch(dir.path() / "m.2.log.gz", "x", old + std::chrono::hours(1));
    LogCompressor c;
    c.submit(dir.path() / "m.gone.log", true, dir.path(), "m", "m.log", 1);
    c.waitIdle();
    EXPECT_EQ(listNames(dir.path()), std::vector<std::string>{"m.2.log.gz"});
}

TEST(FileLoggingTest, CompressorStopDrainsQueueAndIgnoresLaterJobs)
{
    TempDir dir;
    for (int i = 0; i < 5; ++i)
        touch(dir.path() / ("q." + std::to_string(i) + ".log"), "content " + std::to_string(i));

    LogCompressor c;
    for (int i = 0; i < 5; ++i)
        c.submit(dir.path() / ("q." + std::to_string(i) + ".log"), true, dir.path(), "q", "q.log", 0);
    c.stop();
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(fs::exists(dir.path() / ("q." + std::to_string(i) + ".log.gz"))) << i;

    touch(dir.path() / "q.late.log", "late");
    c.submit(dir.path() / "q.late.log", true, dir.path(), "q", "q.log", 0);
    EXPECT_NO_THROW(c.stop()); // idempotent
    EXPECT_TRUE(fs::exists(dir.path() / "q.late.log"));
    EXPECT_FALSE(fs::exists(dir.path() / "q.late.log.gz"));
}

TEST(FileLoggingTest, CompressorDestructorDrains)
{
    TempDir dir;
    touch(dir.path() / "d.1.log", "bye");
    {
        LogCompressor c;
        c.submit(dir.path() / "d.1.log", true, dir.path(), "d", "d.log", 0);
    }
    EXPECT_TRUE(fs::exists(dir.path() / "d.1.log.gz"));
    EXPECT_EQ(gunzip(dir.path() / "d.1.log.gz"), "bye");
}

TEST(FileLoggingTest, FormatLogTimestampShape)
{
    const auto s = formatLogTimestamp(std::chrono::system_clock::now());
    ASSERT_EQ(s.size(), 15u);
    EXPECT_EQ(s[8], '-');
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i != 8)
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(s[i]))) << s;
}

// ---------------------------------------------------------------------------
// installFileLogging — end to end through newLogger()
// ---------------------------------------------------------------------------

class FileLoggingInstallTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        savedDefault_ = spdlog::default_logger();
        console_      = std::make_shared<std::ostringstream>();
        auto sink     = std::make_shared<spdlog::sinks::ostream_sink_mt>(*console_);
        auto def      = std::make_shared<spdlog::logger>("test_default", sink);
        def->set_level(spdlog::level::info);
        spdlog::set_default_logger(def);
    }

    void TearDown() override
    {
        mldp_pvxs_driver::util::log::setLoggerFactory({});
        mldp_pvxs_driver::util::log::setLogger(nullptr);
        spdlog::drop_all();
        if (compressor_)
            compressor_->stop();
        spdlog::set_default_logger(savedDefault_);
    }

    void install(LoggingConfig cfg)
    {
        compressor_ = installFileLogging(cfg, "[%n] %v");
    }

    static void flushAll()
    {
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
        spdlog::default_logger()->flush();
    }

    LoggingConfig cfgFor(const std::string& name = "driver.log")
    {
        LoggingConfig cfg;
        cfg.logFile = (dir_.path() / name).string();
        cfg.console = false;
        return cfg;
    }

    TempDir                                  dir_;
    std::shared_ptr<std::ostringstream>      console_;
    std::shared_ptr<spdlog::logger>          savedDefault_;
    std::shared_ptr<LogCompressor>           compressor_;
};

TEST_F(FileLoggingInstallTest, MainFileOnlyByDefault)
{
    install(cfgFor());
    using mldp_pvxs_driver::util::log::newLogger;
    newLogger("reader:slac-calendar:cal")->log(mldp_pvxs_driver::util::log::Level::Info, "from reader");
    newLogger("controller.default")->log(mldp_pvxs_driver::util::log::Level::Info, "from controller");
    newLogger("configuration_writer:w")->log(mldp_pvxs_driver::util::log::Level::Info, "from writer");
    flushAll();

    const auto names = listNames(dir_.path());
    ASSERT_EQ(names.size(), 1u) << ::testing::PrintToString(names);
    EXPECT_EQ(names[0].rfind("driver.", 0), 0u);
    const auto main = readFile(dir_.path() / names[0]);
    EXPECT_NE(main.find("from reader"), std::string::npos);
    EXPECT_NE(main.find("from controller"), std::string::npos);
    EXPECT_NE(main.find("from writer"), std::string::npos);
}

TEST_F(FileLoggingInstallTest, PerComponentFilesAreNextToLogFileAndHoldOnlyTheirLines)
{
    auto cfg       = cfgFor("sub/driver.log");
    cfg.perReaders = true;
    install(cfg);
    using mldp_pvxs_driver::util::log::Level;
    using mldp_pvxs_driver::util::log::newLogger;
    newLogger("reader:slac-calendar:cal")->log(Level::Info, "reader line");
    newLogger("controller.default")->log(Level::Info, "controller line");
    newLogger("util:http:curl")->log(Level::Info, "http line");
    flushAll();

    const fs::path sub     = dir_.path() / "sub";
    const auto     readers = activeLogs(sub, "reader-slac-calendar_cal");
    ASSERT_EQ(readers.size(), 1u) << ::testing::PrintToString(listNames(sub));
    const auto readerText = readFile(readers[0]);
    EXPECT_NE(readerText.find("reader line"), std::string::npos);
    EXPECT_EQ(readerText.find("controller line"), std::string::npos);
    EXPECT_EQ(readerText.find("http line"), std::string::npos);

    EXPECT_TRUE(activeLogs(sub, "controller-default").empty()); // controller flag off
    const auto main = activeLogs(sub, "driver");
    ASSERT_EQ(main.size(), 1u);
    const auto mainText = readFile(main[0]);
    for (const char* l : {"reader line", "controller line", "http line"})
        EXPECT_NE(mainText.find(l), std::string::npos) << l;
    EXPECT_EQ(listNames(dir_.path()), std::vector<std::string>{"sub"}); // nothing outside log-file folder
}

TEST_F(FileLoggingInstallTest, AllCategoriesGetOwnFiles)
{
    auto cfg          = cfgFor();
    cfg.perReaders    = true;
    cfg.perWriters    = true;
    cfg.perController = true;
    cfg.perRun        = false;
    install(cfg);
    using mldp_pvxs_driver::util::log::Level;
    using mldp_pvxs_driver::util::log::newLogger;
    newLogger("reader:epics-archiver:a")->log(Level::Info, "r");
    newLogger("grpc_writer:g")->log(Level::Info, "w");
    newLogger("controller.default")->log(Level::Info, "c");
    newLogger("mldp_grpc_pool")->log(Level::Info, "pool");
    flushAll();

    EXPECT_EQ(listNames(dir_.path()),
              (std::vector<std::string>{"controller-default.log", "driver.log",
                                        "reader-epics-archiver_a.log", "writer-grpc_g.log"}));
    EXPECT_EQ(readFile(dir_.path() / "writer-grpc_g.log"), "[grpc_writer:g] w\n");
    EXPECT_EQ(readFile(dir_.path() / "controller-default.log"), "[controller.default] c\n");
}

TEST_F(FileLoggingInstallTest, SameLoggerNameSharesOneFile)
{
    auto cfg       = cfgFor();
    cfg.perReaders = true;
    cfg.perRun     = false;
    install(cfg);
    using mldp_pvxs_driver::util::log::Level;
    using mldp_pvxs_driver::util::log::newLogger;
    auto a = newLogger("epics_pvxs_reader");
    auto b = newLogger("epics_pvxs_reader");
    a->log(Level::Info, "one");
    b->log(Level::Info, "two");
    flushAll();
    EXPECT_EQ(readFile(dir_.path() / "reader-epics_pvxs_reader.log"),
              "[epics_pvxs_reader] one\n[epics_pvxs_reader] two\n");
}

TEST_F(FileLoggingInstallTest, ConsoleKeptWhenEnabled)
{
    auto cfg    = cfgFor();
    cfg.console = true;
    install(cfg);
    mldp_pvxs_driver::util::log::newLogger("controller.x")
        ->log(mldp_pvxs_driver::util::log::Level::Info, "to console");
    flushAll();
    EXPECT_NE(console_->str().find("to console"), std::string::npos);
}

TEST_F(FileLoggingInstallTest, ConsoleDroppedWhenDisabled)
{
    install(cfgFor());
    mldp_pvxs_driver::util::log::newLogger("controller.x")
        ->log(mldp_pvxs_driver::util::log::Level::Info, "not on console");
    flushAll();
    EXPECT_EQ(console_->str().find("not on console"), std::string::npos);
}

TEST_F(FileLoggingInstallTest, LevelOfPreviousDefaultIsInherited)
{
    spdlog::default_logger()->set_level(spdlog::level::warn);
    auto cfg   = cfgFor();
    cfg.perRun = false;
    install(cfg);
    using mldp_pvxs_driver::util::log::Level;
    auto l = mldp_pvxs_driver::util::log::newLogger("controller.lvl");
    l->log(Level::Info, "hidden info");
    l->log(Level::Warn, "shown warn");
    EXPECT_FALSE(l->shouldLog(Level::Info));
    flushAll();
    const auto text = readFile(dir_.path() / "driver.log");
    EXPECT_EQ(text.find("hidden info"), std::string::npos);
    EXPECT_NE(text.find("shown warn"), std::string::npos);
}

TEST_F(FileLoggingInstallTest, DefaultAndGlobalLoggerGoToMainFile)
{
    auto cfg   = cfgFor();
    cfg.perRun = false;
    install(cfg);
    spdlog::info("via spdlog default");
    mldp_pvxs_driver::util::log::getLogger().log(mldp_pvxs_driver::util::log::Level::Info,
                                                 "via library global");
    mldp_pvxs_driver::util::log::newLogger("")->log(mldp_pvxs_driver::util::log::Level::Info,
                                                    "via empty name");
    flushAll();
    const auto text = readFile(dir_.path() / "driver.log");
    EXPECT_NE(text.find("[test_default] via spdlog default"), std::string::npos);
    EXPECT_NE(text.find("via library global"), std::string::npos);
    EXPECT_NE(text.find("via empty name"), std::string::npos);
}

TEST_F(FileLoggingInstallTest, PerRunTagUsedForAllFiles)
{
    auto cfg       = cfgFor();
    cfg.perReaders = true;
    install(cfg);
    mldp_pvxs_driver::util::log::newLogger("reader:t:r")
        ->log(mldp_pvxs_driver::util::log::Level::Info, "x");
    flushAll();

    const auto main   = activeLogs(dir_.path(), "driver");
    const auto reader = activeLogs(dir_.path(), "reader-t_r");
    ASSERT_EQ(main.size(), 1u);
    ASSERT_EQ(reader.size(), 1u);
    const auto tagOf = [](const fs::path& p, const std::string& stem)
    { return p.filename().string().substr(stem.size() + 1, 15); };
    EXPECT_EQ(tagOf(main[0], "driver"), tagOf(reader[0], "reader-t_r"));
}

TEST_F(FileLoggingInstallTest, PreviousRunFilesCompressedAtInstall)
{
    touch(dir_.path() / "driver.20200101-000000.log", "previous run\n");
    install(cfgFor());
    compressor_->waitIdle();
    EXPECT_TRUE(fs::exists(dir_.path() / "driver.20200101-000000.log.gz"));
    EXPECT_EQ(gunzip(dir_.path() / "driver.20200101-000000.log.gz"), "previous run\n");
}

TEST(FileLoggingTest, ConfigEmptyLoggingBlockIsConsoleOnly)
{
    const auto cfg = LoggingConfig::parse(makeConfigFromYaml("logging:\ncontroller:\n  name: x\n"));
    EXPECT_FALSE(cfg.fileLoggingEnabled());
    EXPECT_TRUE(cfg.console);
}

TEST(FileLoggingTest, SameSecondRotationDoesNotOverwriteCompressedFile)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 5;
    policy.maxFiles = 0;
    const auto fixed = std::chrono::system_clock::now();
    auto       sink  = std::make_shared<RotatingGzipFileSink>(dir.path(), "ow", "", policy, compressor,
                                                       [fixed] { return fixed; });
    auto logger = makeLogger(sink);
    for (int i = 0; i < 4; ++i)
    {
        logger->info("line{}", i);
        logger->flush();
        compressor->waitIdle(); // earlier rotated file is already .gz before the next rotation
    }

    const auto  names = listNames(dir.path());
    std::string archived;
    for (const auto& n : names)
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".gz") == 0)
            archived += gunzip(dir.path() / n);
    EXPECT_EQ(countSuffix(names, ".log.gz"), 3) << ::testing::PrintToString(names);
    for (int i = 0; i < 3; ++i)
        EXPECT_NE(archived.find("line" + std::to_string(i)), std::string::npos) << i;
    EXPECT_EQ(readFile(dir.path() / "ow.log"), "line3\n");
}

// ---------------------------------------------------------------------------
// Concurrency: many threads logging at the same time
// ---------------------------------------------------------------------------

namespace {

/// All content of `<stem>.*` files (plain + gz) and `<stem>.log` in @p dir.
std::string familyContent(const fs::path& dir, const std::string& stem)
{
    std::string out;
    for (const auto& e : fs::directory_iterator(dir))
    {
        const auto n = e.path().filename().string();
        if (n.rfind(stem + ".", 0) != 0)
            continue;
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".gz") == 0)
            out += gunzip(e.path());
        else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
            out += readFile(e.path());
    }
    return out;
}

/// Parse lines of form "<prefix>T<t>-<i>" and count each (t,i). Returns false on a malformed line.
bool countLines(const std::string& text, const std::string& prefix, std::map<std::string, int>& seen,
                std::string& bad)
{
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const auto pos = line.find(prefix);
        if (pos == std::string::npos)
            continue; // other logger's line (main file)
        const auto id = line.substr(pos + prefix.size());
        if (id.empty() || id[0] != 'T' || id.find('-') == std::string::npos ||
            id.find_first_not_of("T0123456789-") != std::string::npos)
        {
            bad = line;
            return false;
        }
        ++seen[id];
    }
    return true;
}

} // namespace

TEST(FileLoggingTest, ConcurrentWritersOneSinkWithRotationLoseNothing)
{
    TempDir           dir;
    auto              compressor = std::make_shared<LogCompressor>();
    LogRotationPolicy policy;
    policy.maxBytes = 4096;
    policy.maxFiles = 0;
    auto sink   = std::make_shared<RotatingGzipFileSink>(dir.path(), "mt", "", policy, compressor);
    auto logger = makeLogger(sink);

    constexpr int kThreads = 8;
    constexpr int kLines   = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kLines; ++i)
                logger->info("msg T{}-{}", t, i);
        });
    for (auto& th : threads)
        th.join();
    logger->flush();
    compressor->waitIdle();

    EXPECT_GT(countSuffix(listNames(dir.path()), ".log.gz"), 10);
    std::map<std::string, int> seen;
    std::string                bad;
    ASSERT_TRUE(countLines(familyContent(dir.path(), "mt"), "msg ", seen, bad)) << "corrupt line: " << bad;
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kThreads * kLines));
    for (const auto& [id, n] : seen)
        ASSERT_EQ(n, 1) << id;
}

TEST_F(FileLoggingInstallTest, ConcurrentComponentsAndLoggerCreation)
{
    auto cfg          = cfgFor();
    cfg.perReaders    = true;
    cfg.perController = true;
    cfg.perRun        = false;
    cfg.rotation.maxBytes = 8192;
    cfg.rotation.maxFiles = 0;
    install(cfg);

    constexpr int kThreads = 6;
    constexpr int kLines   = 1000;
    // Even threads: one reader each; odd threads: all share the same controller logger name,
    // created concurrently via newLogger() from every thread.
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([t] {
            const std::string name = (t % 2 == 0) ? "reader:mt:r" + std::to_string(t) : "controller.shared";
            for (int i = 0; i < kLines; ++i)
            {
                auto l = mldp_pvxs_driver::util::log::newLogger(name); // concurrent lookup/creation
                l->log(mldp_pvxs_driver::util::log::Level::Info,
                       "msg T" + std::to_string(t) + "-" + std::to_string(i));
            }
        });
    for (auto& th : threads)
        th.join();
    flushAll();
    compressor_->waitIdle();

    std::string bad;

    // Main file: every line exactly once.
    std::map<std::string, int> main;
    ASSERT_TRUE(countLines(familyContent(dir_.path(), "driver"), "msg ", main, bad)) << bad;
    EXPECT_EQ(main.size(), static_cast<std::size_t>(kThreads * kLines));
    for (const auto& [id, n] : main)
        ASSERT_EQ(n, 1) << id;

    // Each reader file: only its own thread's lines, all of them.
    for (int t = 0; t < kThreads; t += 2)
    {
        std::map<std::string, int> r;
        const auto text = familyContent(dir_.path(), "reader-mt_r" + std::to_string(t));
        ASSERT_TRUE(countLines(text, "msg ", r, bad)) << bad;
        EXPECT_EQ(r.size(), static_cast<std::size_t>(kLines)) << t;
        for (const auto& [id, n] : r)
        {
            EXPECT_EQ(id.rfind("T" + std::to_string(t) + "-", 0), 0u) << id;
            ASSERT_EQ(n, 1) << id;
        }
    }

    // Shared controller file: lines of all odd threads, once each.
    std::map<std::string, int> c;
    ASSERT_TRUE(countLines(familyContent(dir_.path(), "controller-shared"), "msg ", c, bad)) << bad;
    EXPECT_EQ(c.size(), static_cast<std::size_t>((kThreads / 2) * kLines));
    for (const auto& [id, n] : c)
        ASSERT_EQ(n, 1) << id;
}
