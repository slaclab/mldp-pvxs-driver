//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <cli/RotatingGzipFileSink.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace mldp_pvxs_driver::cli {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string formatLogTimestamp(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm           utc{};
    gmtime_r(&t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &utc);
    return buf;
}

namespace {

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// LogCompressor
// ---------------------------------------------------------------------------

LogCompressor::LogCompressor()
    : worker_([this] { run(); })
{
}

LogCompressor::~LogCompressor()
{
    stop();
}

void LogCompressor::submit(fs::path file, bool compress, fs::path dir, std::string stem,
                           std::string keepName, int maxFiles)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stopping_)
            return;
        jobs_.push_back(Job{std::move(file), compress, std::move(dir), std::move(stem),
                            std::move(keepName), maxFiles});
    }
    cv_.notify_one();
}

void LogCompressor::stop()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stopping_ && !worker_.joinable())
            return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void LogCompressor::waitIdle()
{
    std::unique_lock<std::mutex> lk(mutex_);
    idleCv_.wait(lk, [this] { return jobs_.empty() && !busy_; });
}

void LogCompressor::run()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty())
            {
                // stopping_ and fully drained.
                idleCv_.notify_all();
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }

        std::error_code ec;
        if (job.compress && !job.file.empty() && fs::exists(job.file, ec))
        {
            const fs::path gz = job.file.string() + ".gz";
            if (gzipFile(job.file, gz))
                fs::remove(job.file, ec);
        }
        prune(job.dir, job.stem, job.keepName, job.maxFiles);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            busy_ = false;
            if (jobs_.empty())
                idleCv_.notify_all();
        }
    }
}

bool LogCompressor::gzipFile(const fs::path& src, const fs::path& dst)
{
    std::ifstream in(src, std::ios::binary);
    if (!in)
        return false;

    const fs::path tmp = dst.string() + ".tmp";
    gzFile         out = gzopen(tmp.c_str(), "wb6");
    if (!out)
        return false;

    std::array<char, 64 * 1024> buf{};
    bool                        ok = true;
    while (in)
    {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0 && gzwrite(out, buf.data(), static_cast<unsigned>(n)) != static_cast<int>(n))
        {
            ok = false;
            break;
        }
    }
    if (gzclose(out) != Z_OK)
        ok = false;

    std::error_code ec;
    if (!ok)
    {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, dst, ec);
    return !ec;
}

void LogCompressor::prune(const fs::path& dir, const std::string& stem,
                          const std::string& keepName, int maxFiles)
{
    if (maxFiles <= 0)
        return;

    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        const std::string name = entry.path().filename().string();
        if (name == keepName || !startsWith(name, stem + ".") || endsWith(name, ".tmp"))
            continue;
        if (!endsWith(name, ".log") && !endsWith(name, ".log.gz"))
            continue;
        files.emplace_back(entry.last_write_time(ec), entry.path());
    }
    if (static_cast<int>(files.size()) <= maxFiles)
        return;

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::size_t excess = files.size() - static_cast<std::size_t>(maxFiles);
    for (std::size_t i = 0; i < excess; ++i)
        fs::remove(files[i].second, ec);
}

// ---------------------------------------------------------------------------
// RotatingGzipFileSink
// ---------------------------------------------------------------------------

RotatingGzipFileSink::RotatingGzipFileSink(fs::path dir, std::string stem, std::string runTag,
                                           LogRotationPolicy policy,
                                           std::shared_ptr<LogCompressor> compressor, Clock clock)
    : dir_(std::move(dir))
    , stem_(std::move(stem))
    , activeName_(stem_ + (runTag.empty() ? "" : "." + runTag) + ".log")
    , policy_(policy)
    , compressor_(std::move(compressor))
    , clock_(clock ? std::move(clock) : Clock([] { return std::chrono::system_clock::now(); }))
{
    fs::create_directories(dir_);

    // Previous runs' plain .log files of this family: compress and prune them.
    if (compressor_)
    {
        std::error_code ec;
        bool            queued = false;
        for (const auto& entry : fs::directory_iterator(dir_, ec))
        {
            const std::string name = entry.path().filename().string();
            if (name != activeName_ && startsWith(name, stem_ + ".") && endsWith(name, ".log"))
            {
                compressor_->submit(entry.path(), policy_.compress, dir_, stem_, activeName_,
                                    policy_.maxFiles);
                queued = true;
            }
        }
        if (!queued)
            compressor_->submit({}, false, dir_, stem_, activeName_, policy_.maxFiles);
    }

    file_.open(activePath().string(), false);
    currentSize_ = file_.size();
    openedAt_    = clock_();
}

fs::path RotatingGzipFileSink::activePath() const
{
    return dir_ / activeName_;
}

void RotatingGzipFileSink::sink_it_(const spdlog::details::log_msg& msg)
{
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);

    const auto size     = currentSize_;
    const bool sizeFull = policy_.maxBytes > 0 && size > 0 &&
                          size + formatted.size() > policy_.maxBytes;
    const bool tooOld   = policy_.maxAge.count() > 0 && clock_() - openedAt_ >= policy_.maxAge;
    if (sizeFull || tooOld)
        rotate_();

    file_.write(formatted);
    currentSize_ += formatted.size();
}

void RotatingGzipFileSink::flush_()
{
    file_.flush();
}

void RotatingGzipFileSink::rotate_()
{
    file_.close();

    const std::string base = activeName_.substr(0, activeName_.size() - 4); // drop ".log"
    const std::string ts   = formatLogTimestamp(clock_());
    fs::path          rotated = dir_ / (base + "." + ts + ".log");
    std::error_code   ec;
    // A name is taken if the plain file, its archive, or an in-progress archive
    // exists: an earlier rotation in the same second may already be compressed,
    // and reusing its name would overwrite that .gz.
    const auto taken = [&ec](const fs::path& p)
    {
        return fs::exists(p, ec) || fs::exists(p.string() + ".gz", ec) ||
               fs::exists(p.string() + ".gz.tmp", ec);
    };
    for (int n = 1; taken(rotated); ++n)
        rotated = dir_ / (base + "." + ts + "." + std::to_string(n) + ".log");

    fs::rename(activePath(), rotated, ec);
    file_.open(activePath().string(), true);
    currentSize_ = 0;
    openedAt_    = clock_();

    if (!ec && compressor_)
        compressor_->submit(rotated, policy_.compress, dir_, stem_, activeName_, policy_.maxFiles);
}

} // namespace mldp_pvxs_driver::cli
