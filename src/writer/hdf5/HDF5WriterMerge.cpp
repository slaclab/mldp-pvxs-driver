//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/hdf5/HDF5WriterMerge.h>
#include "HDF5WriterDetail.h"

#include <util/log/Logger.h>

#include <ctime>
#include <filesystem>

using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::writer::hdf5_detail;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HDF5WriterMerge::HDF5WriterMerge(HDF5WriterConfig                  config,
                                   std::shared_ptr<metrics::Metrics> metrics)
    : HDF5WriterBase(std::move(config), std::move(metrics))
{
}

HDF5WriterMerge::HDF5WriterMerge(const config::Config&             node,
                                   std::shared_ptr<metrics::Metrics> metrics)
    : HDF5WriterMerge(HDF5WriterConfig::parse(node), std::move(metrics))
{
}

HDF5WriterMerge::~HDF5WriterMerge()
{
    if (!stopping_.load())
        stop();
}

// ---------------------------------------------------------------------------
// doStart / doStop / doFlushAll
// ---------------------------------------------------------------------------

void HDF5WriterMerge::doStart()
{
    openMergeFile();
}

void HDF5WriterMerge::doStop() noexcept
{
    closeMergeFile();
}

void HDF5WriterMerge::doFlushAll() noexcept
{
    if (mergeFile_)
    {
        std::lock_guard<std::mutex> lk(mergeFileMutex_);
        try { mergeFile_->flush(H5F_SCOPE_GLOBAL); } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// writeFrameImpl / flushTabularBufferImpl
// ---------------------------------------------------------------------------

void HDF5WriterMerge::writeFrameImpl(const std::string&          source,
                                      const util::bus::DataBatch& frame,
                                      uint64_t                    batchSeq)
{
    appendFrameMerge(source, frame, batchSeq);
}

void HDF5WriterMerge::flushTabularBufferImpl(const std::string& source,
                                              TabularBuffer&     buf)
{
    flushTabularBufferMerge(source, buf);
}

// ---------------------------------------------------------------------------
// openMergeFile / closeMergeFile / rotateMergeFile
// ---------------------------------------------------------------------------

void HDF5WriterMerge::openMergeFile()
{
    const auto        now = std::chrono::system_clock::now();
    const std::time_t t   = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char tsbuf[20];
    std::strftime(tsbuf, sizeof(tsbuf), "%Y%m%dT%H%M%Sz", &utc);
    const std::string suffix = std::string(tsbuf) + "_" + std::to_string(mergeFileSeq_++);

    const std::filesystem::path base(config_.basePath);
    std::filesystem::create_directories(base);

    const std::string stem = "merged";
    mergePath_      = base / ("." + stem + "_" + suffix + ".hdf5");
    mergeFinalPath_ = base / (stem + "_" + suffix + ".hdf5");

    std::lock_guard<std::mutex> lk(mergeFileMutex_);
    mergeFile_ = std::make_unique<H5::H5File>(mergePath_.string(), H5F_ACC_TRUNC);
    mergeFileOpenedAt_ = std::chrono::steady_clock::now();
    mergeBytesWritten_ = 0;
    mergeOpenGroups_.clear();
    infof(*logger_, "HDF5Writer [{}] merge file opened: {}", config_.name, mergePath_.string());
}

void HDF5WriterMerge::closeMergeFile() noexcept
{
    std::unique_lock<std::mutex> lk(mergeFileMutex_);
    if (!mergeFile_) return;
    try
    {
        mergeFile_->flush(H5F_SCOPE_GLOBAL);
        mergeFile_->close();
        mergeFile_.reset();
        lk.unlock();

        if (mergePath_ != mergeFinalPath_)
        {
            std::error_code ec;
            std::filesystem::rename(mergePath_, mergeFinalPath_, ec);
            if (ec)
                warnf(*logger_, "HDF5Writer [{}] merge file rename failed: {} -> {}: {}",
                      config_.name, mergePath_.string(), mergeFinalPath_.string(), ec.message());
            else
                debugf(*logger_, "HDF5Writer [{}] merge file renamed -> {}",
                       config_.name, mergeFinalPath_.string());
            mergePath_ = mergeFinalPath_;
        }
    }
    catch (const H5::Exception& ex)
    {
        errorf(*logger_, "HDF5Writer [{}] merge file close HDF5 error: {}",
               config_.name, ex.getCDetailMsg());
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "HDF5Writer [{}] merge file close failed: {}",
               config_.name, ex.what());
    }
    catch (...)
    {
        errorf(*logger_, "HDF5Writer [{}] merge file close failed — unknown exception",
               config_.name);
    }
}

void HDF5WriterMerge::rotateMergeFile()
{
    infof(*logger_, "HDF5Writer [{}] rotating merge file", config_.name);
    std::set<std::string> groupsToRecreate;
    {
        std::lock_guard<std::mutex> lk(mergeFileMutex_);
        groupsToRecreate = mergeOpenGroups_;
    }
    closeMergeFile();
    openMergeFile();

    std::lock_guard<std::mutex> lk(mergeFileMutex_);
    for (const auto& g : groupsToRecreate)
    {
        if (!mergeFile_->nameExists(g))
        {
            mergeFile_->createGroup(g);
            mergeOpenGroups_.insert(g);
        }
    }
    infof(*logger_, "HDF5Writer [{}] merge file rotated, {} groups recreated",
          config_.name, groupsToRecreate.size());
}

void HDF5WriterMerge::ensureMergeGroup(const std::string& sourceName)
{
    // Caller MUST hold mergeFileMutex_.
    if (mergeOpenGroups_.find(sourceName) == mergeOpenGroups_.end())
    {
        if (!mergeFile_->nameExists(sourceName))
        {
            mergeFile_->createGroup(sourceName);
            infof(*logger_, "HDF5Writer [{}] merge group created: /{}/",
                  config_.name, sourceName);
        }
        mergeOpenGroups_.insert(sourceName);
    }
}

void HDF5WriterMerge::checkMergeRotation()
{
    bool needRotate = false;
    {
        std::lock_guard<std::mutex> lk(mergeFileMutex_);
        if (!mergeFile_) return;
        const auto     now = std::chrono::steady_clock::now();
        const auto     age = std::chrono::duration_cast<std::chrono::seconds>(now - mergeFileOpenedAt_);
        const uint64_t sizeLimitBytes =
            static_cast<uint64_t>(config_.maxFileSizeMB) * 1024ULL * 1024ULL;
        needRotate = (age >= config_.maxFileAge) ||
                     (sizeLimitBytes > 0 && mergeBytesWritten_ >= sizeLimitBytes);
    }
    if (needRotate)
    {
        bool expected = false;
        if (mergeRotating_.compare_exchange_strong(expected, true))
        {
            rotateMergeFile();
            mergeRotating_.store(false);
        }
    }
}

// ---------------------------------------------------------------------------
// appendFrameMerge
// ---------------------------------------------------------------------------

void HDF5WriterMerge::appendFrameMerge(const std::string&          sourceName,
                                        const util::bus::DataBatch& batch,
                                        uint64_t                    batchSeq)
{
    if (batch.timestamps.empty())
    {
        debugf(*logger_, "HDF5Writer appendFrameMerge source={} — no timestamps, skipping", sourceName);
        return;
    }

    checkMergeRotation();

    const std::size_t           tsCount = batch.timestamps.size();
    std::lock_guard<std::mutex> lk(mergeFileMutex_);

    if (!mergeFile_)
    {
        warnf(*logger_, "HDF5Writer [{}] appendFrameMerge — merge file not open, skipping", config_.name);
        return;
    }

    ensureMergeGroup(sourceName);
    const std::string groupPrefix = sourceName + "/";

    // 1. timestamps
    {
        auto it = lastTsBatchSeq_.find(sourceName);
        if (it == lastTsBatchSeq_.end() || it->second != batchSeq)
        {
            std::vector<int64_t> nsVec;
            nsVec.reserve(tsCount);
            for (const auto& ts : batch.timestamps)
            {
                nsVec.push_back(
                    static_cast<int64_t>(ts.epoch_seconds) * 1'000'000'000LL +
                    static_cast<int64_t>(ts.nanoseconds));
            }
            auto ds = ensureDataset(*mergeFile_, groupPrefix + "timestamps", H5::PredType::NATIVE_INT64);
            append1D(ds, H5::PredType::NATIVE_INT64, nsVec.data(), static_cast<hsize_t>(tsCount));
            lastTsBatchSeq_[sourceName] = batchSeq;
        }
    }

    // 2. Columns
    writeColumnsImpl(batch,
        [&](const std::string& n, const H5::DataType& t)
            { return ensureDataset(*mergeFile_, groupPrefix + n, t); },
        [&](const std::string& n, const H5::DataType& t, hsize_t l)
            { return ensureDataset2D(*mergeFile_, groupPrefix + n, t, l); },
        [&](uint64_t bytes) { mergeBytesWritten_ += bytes; }
    );
}

// ---------------------------------------------------------------------------
// flushTabularBufferMerge
// ---------------------------------------------------------------------------

void HDF5WriterMerge::flushTabularBufferMerge(const std::string& sourceName,
                                               TabularBuffer&     buf)
{
    checkMergeRotation();

    std::lock_guard<std::mutex> lk(mergeFileMutex_);
    if (!mergeFile_)
    {
        warnf(*logger_, "HDF5Writer [{}] flushTabularBufferMerge — merge file not open, skipping",
              config_.name);
        return;
    }

    const std::size_t nRows = buf.rowCount;
    const std::size_t nCols = buf.colNames.size();

    ensureMergeGroup(sourceName);
    flushTabularBuffer(sourceName, buf, *mergeFile_);

    if (nRows > 0 && nCols > 0)
    {
        const uint64_t approxBytes =
            static_cast<uint64_t>(nRows) *
            (static_cast<uint64_t>(nCols) * sizeof(double) + 2ULL * sizeof(int64_t));
        mergeBytesWritten_ += approxBytes;
    }
}
