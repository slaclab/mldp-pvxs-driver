//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cli/LoggingConfig.h>

#include <spdlog/details/file_helper.h>
#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mldp_pvxs_driver::cli {

/**
 * @brief Background worker that gzips rotated log files and prunes old ones.
 *
 * One instance is shared by all sinks. Jobs run sequentially on a single thread;
 * stop() (also called by the destructor) drains every queued job before returning.
 */
class LogCompressor
{
public:
    LogCompressor();
    ~LogCompressor();

    LogCompressor(const LogCompressor&)            = delete;
    LogCompressor& operator=(const LogCompressor&) = delete;

    /**
     * @brief Queue a finished log file.
     *
     * @param file      File to compress (skipped when @p compress is false).
     * @param compress  gzip @p file to `<file>.gz` and remove the original.
     * @param dir       Directory holding the log family.
     * @param stem      Family stem; files named `<stem>.*` are candidates for pruning.
     * @param keepName  Active file name never pruned.
     * @param maxFiles  Keep at most this many non-active files of the family (0 = unlimited).
     */
    void submit(std::filesystem::path file, bool compress, std::filesystem::path dir,
                std::string stem, std::string keepName, int maxFiles);

    /// Drain all pending jobs and stop the worker thread. Idempotent.
    void stop();

    /// Block until every job queued so far has completed (used by tests).
    void waitIdle();

    /// gzip @p src into @p dst. Returns false on I/O error.
    static bool gzipFile(const std::filesystem::path& src, const std::filesystem::path& dst);

    /// Remove the oldest `<stem>.*` files in @p dir beyond @p maxFiles, never touching @p keepName.
    static void prune(const std::filesystem::path& dir, const std::string& stem,
                      const std::string& keepName, int maxFiles);

private:
    struct Job
    {
        std::filesystem::path file;
        bool                  compress;
        std::filesystem::path dir;
        std::string           stem;
        std::string           keepName;
        int                   maxFiles;
    };

    void run();

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::deque<Job>         jobs_;
    bool                    busy_{false};
    bool                    stopping_{false};
    std::thread             worker_;
};

/**
 * @brief spdlog file sink with size/age rotation and background gzip.
 *
 * Active file: `<dir>/<stem>[.<runTag>].log`. On rotation it is renamed to
 * `<dir>/<stem>[.<runTag>].<YYYYmmdd-HHMMSS>[.N].log`, reopened empty, and the
 * rotated file is handed to the LogCompressor. At construction, leftover
 * `<stem>.*.log` files from previous runs are queued for compression too.
 */
class RotatingGzipFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    RotatingGzipFileSink(std::filesystem::path dir, std::string stem, std::string runTag,
                         LogRotationPolicy policy, std::shared_ptr<LogCompressor> compressor,
                         Clock clock = {});

    /// Path of the active file.
    std::filesystem::path activePath() const;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    void rotate_();

    std::filesystem::path          dir_;
    std::string                    stem_;
    std::string                    activeName_;
    LogRotationPolicy              policy_;
    std::shared_ptr<LogCompressor> compressor_;
    Clock                          clock_;
    spdlog::details::file_helper   file_;
    std::chrono::system_clock::time_point openedAt_;
    std::size_t                    currentSize_{0}; ///< Bytes in active file (incl. unflushed).
};

} // namespace mldp_pvxs_driver::cli
