//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "HDF5WriterDetail.h"
#include <writer/hdf5/HDF5WriterPerSource.h>

#include <util/log/Logger.h>

using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::writer::hdf5_detail;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HDF5WriterPerSource::HDF5WriterPerSource(HDF5WriterConfig                  config,
                                         std::shared_ptr<metrics::Metrics> metrics)
    : HDF5WriterBase(std::move(config), std::move(metrics))
{
}

HDF5WriterPerSource::HDF5WriterPerSource(const config::Config&             node,
                                         std::shared_ptr<metrics::Metrics> metrics)
    : HDF5WriterPerSource(HDF5WriterConfig::parse(node), std::move(metrics))
{
}

HDF5WriterPerSource::~HDF5WriterPerSource()
{
    if (!stopping_.load())
        stop();
}

// ---------------------------------------------------------------------------
// doStart / doStop / doFlushAll
// ---------------------------------------------------------------------------

void HDF5WriterPerSource::doStart()
{
    pool_ = std::make_unique<HDF5FilePool>(config_);
    if (writerMetrics_)
        pool_->setMetrics(writerMetrics_.get());
}

void HDF5WriterPerSource::doStop() noexcept
{
    if (pool_)
    {
        pool_->closeAll();
        pool_.reset();
    }
}

void HDF5WriterPerSource::doFlushAll() noexcept
{
    if (pool_)
        pool_->flushAll();
}

// ---------------------------------------------------------------------------
// writeFrameImpl
// ---------------------------------------------------------------------------

void HDF5WriterPerSource::writeFrameImpl(const std::string&          source,
                                         const util::bus::DataBatch& frame,
                                         uint64_t                    batchSeq)
{
    auto ev = pool_->acquire(source);

    const uint64_t written = static_cast<uint64_t>(
        frame.timestamps.size() * frame.columns.size() * sizeof(double));

    {
        std::lock_guard<std::mutex> fileLk(ev->fileMutex);
        appendFrame(source, frame, ev->file, batchSeq);
    }

    tracef(*logger_, "HDF5Writer [{}] source={} wrote ~{} bytes", config_.name, source, written);

    if (written > 0)
    {
        pool_->recordWrite(source, written);
        if (writerMetrics_)
        {
            writerMetrics_->incrementBytesWritten(source, static_cast<double>(written));
            writerMetrics_->incrementRowsWritten(source, static_cast<double>(frame.timestamps.size()));
        }
    }
}

// ---------------------------------------------------------------------------
// flushTabularBufferImpl
// ---------------------------------------------------------------------------

void HDF5WriterPerSource::flushTabularBufferImpl(const std::string& source,
                                                 TabularBuffer&     buf)
{
    auto                        ev = pool_->acquire(source);
    std::lock_guard<std::mutex> fileLk(ev->fileMutex);
    flushTabularBuffer(source, buf, ev->file);
}

// ---------------------------------------------------------------------------
// appendFrame
// ---------------------------------------------------------------------------

void HDF5WriterPerSource::appendFrame(const std::string&          sourceName,
                                      const util::bus::DataBatch& batch,
                                      H5::H5File&                 file,
                                      uint64_t                    batchSeq)
{
    if (batch.timestamps.empty())
    {
        debugf(*logger_, "HDF5Writer appendFrame source={} — batch has no timestamps, skipping", sourceName);
        return;
    }
    const std::size_t tsCount = batch.timestamps.size();

    // 1. timestamps dataset
    {
        auto it = lastTsBatchSeq_.find(sourceName);
        if (it != lastTsBatchSeq_.end() && it->second == batchSeq)
        {
            tracef(*logger_,
                   "HDF5Writer appendFrame source={} batchSeq={} — " "timestamps already written (split-column frame), skipping",
                   sourceName, batchSeq);
        }
        else
        {
            std::vector<int64_t> nsVec;
            nsVec.reserve(tsCount);
            for (const auto& ts : batch.timestamps)
            {
                nsVec.push_back(
                    static_cast<int64_t>(ts.epoch_seconds) * 1'000'000'000LL +
                    static_cast<int64_t>(ts.nanoseconds));
            }
            auto ds = ensureDataset(file, "timestamps", H5::PredType::NATIVE_INT64);
            append1D(ds, H5::PredType::NATIVE_INT64, nsVec.data(), static_cast<hsize_t>(tsCount));
            lastTsBatchSeq_[sourceName] = batchSeq;
        }
    }

    // 2. Scalar and array columns via writeColumnsImpl
    writeColumnsImpl(batch, [&](const std::string& n, const H5::DataType& t)
                     {
                         return ensureDataset(file, n, t);
                     },
                     [&](const std::string& n, const H5::DataType& t, hsize_t l)
                     {
                         return ensureDataset2D(file, n, t, l);
                     },
                     [](uint64_t) {} // byte accounting done by pool_->recordWrite in writeFrameImpl
    );
}
