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
#include <writer/hdf5/HDF5WriterBase.h>

#include <BS_thread_pool.hpp>
#include <util/log/Logger.h>
#include <writer/hdf5/HDF5WriterMetrics.h>

#include <vector>

using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::writer::hdf5_detail;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

HDF5WriterBase::HDF5WriterBase(HDF5WriterConfig                  config,
                               std::shared_ptr<metrics::Metrics> metrics)
    : config_(std::move(config))
    , logger_(util::log::newLogger("hdf5_writer:" + config_.name))
{
    if (metrics)
    {
        writerMetrics_ = std::make_unique<metrics::HDF5WriterMetrics>(
            *metrics->registry(), metrics->controllerName(), config_.name);
    }
}

HDF5WriterBase::~HDF5WriterBase()
{
    // Do NOT call stop() here — doStop() is pure virtual and the subclass
    // vtable has already been torn down by the time this base dtor runs.
    // Each subclass calls stop() from its own destructor while the vtable
    // is still valid.
}

// ---------------------------------------------------------------------------
// IWriter lifecycle
// ---------------------------------------------------------------------------

void HDF5WriterBase::start()
{
    infof(*logger_, "HDF5Writer [{}] starting (output_dir={}, max_file_size_mb={}, flush_interval_ms={})",
          config_.name,
          config_.basePath,
          config_.maxFileSizeMB,
          std::chrono::duration_cast<std::chrono::milliseconds>(config_.flushInterval).count());

    stopping_.store(false);
    doStart();

    writerThread_ = std::thread([this]
                                {
                                    BS::this_thread::set_os_thread_name("hdf5-writer");
                                    writerLoop();
                                });
    flushThread_ = std::thread([this]
                               {
                                   BS::this_thread::set_os_thread_name("hdf5-flush");
                                   flushLoop();
                               });

    infof(*logger_, "HDF5Writer [{}] started — writer and flush threads running", config_.name);
}

void HDF5WriterBase::stop() noexcept
{
    infof(*logger_, "HDF5Writer [{}] stopping", config_.name);

    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        stopping_.store(true);
    }
    queueCv_.notify_all();

    if (writerThread_.joinable())
    {
        try
        {
            writerThread_.join();
        }
        catch (...)
        {
        }
    }
    if (flushThread_.joinable())
    {
        try
        {
            tracef(*logger_, "HDF5Writer [{}] [tid={}] joining flush thread", config_.name, std::this_thread::get_id());
            flushThread_.join();
            tracef(*logger_, "HDF5Writer [{}] [tid={}] flush thread joined", config_.name, std::this_thread::get_id());
        }
        catch (...)
        {
        }
    }

    doStop();

    infof(*logger_, "HDF5Writer [{}] stopped", config_.name);
}

// ---------------------------------------------------------------------------
// push()
// ---------------------------------------------------------------------------

bool HDF5WriterBase::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (stopping_.load())
    {
        debugf(*logger_, "HDF5Writer [{}] push rejected — writer is stopping", config_.name);
        return false;
    }
    const uint64_t              seq = nextBatchSeq_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (queue_.size() >= kQueueCapacity)
    {
        warnf(*logger_, "HDF5Writer [{}] queue full ({} items) — dropping batch", config_.name, queue_.size());
        if (writerMetrics_)
            writerMetrics_->incrementQueueDrops();
        return false;
    }
    queue_.push_back({seq, std::move(batch)});
    queueCv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// writerLoop()
// ---------------------------------------------------------------------------

void HDF5WriterBase::writerLoop()
{
    debugf(*logger_, "HDF5Writer [{}] writer thread started", config_.name);
    while (true)
    {
        std::deque<QueueEntry> drained;
        std::size_t            depthAtDrain = 0;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this]
                          {
                              return !queue_.empty() || stopping_.load();
                          });
            if (queue_.empty())
            {
                debugf(*logger_, "HDF5Writer [{}] writer thread exiting — queue drained", config_.name);
                break;
            }
            depthAtDrain = queue_.size();
            drained.swap(queue_);
        }

        if (writerMetrics_)
            writerMetrics_->setQueueDepth(static_cast<double>(depthAtDrain));

        for (auto& entry : drained)
        {
            try
            {
                if (!util::bus::isTimeSeries(entry.batch))
                {
                    continue;
                }
                const auto& ts = util::bus::asTimeSeries(entry.batch);
                if (ts.end_of_batch_group)
                {
                    const auto& source = ts.root_source_name;
                    auto        it = tabularBuffers_.find(source);
                    if (it != tabularBuffers_.end() && it->second.rowCount > 0)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        flushTabularBufferImpl(source, it->second);
                        if (writerMetrics_)
                        {
                            const double ms = std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - t0)
                                                  .count();
                            writerMetrics_->observeWriteLatencyMs(ms);
                            writerMetrics_->incrementBatchesWritten();
                        }
                    }
                }
                else if (ts.is_tabular)
                {
                    processTabularBatch(entry);
                }
                else
                {
                    for (const auto& frame : ts.frames)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        writeFrameImpl(ts.root_source_name, frame, entry.batchSeq);
                        if (writerMetrics_)
                        {
                            const double ms = std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - t0)
                                                  .count();
                            writerMetrics_->observeWriteLatencyMs(ms);
                        }
                    }
                    if (writerMetrics_)
                        writerMetrics_->incrementBatchesWritten();
                }
            }
            catch (const H5::Exception& ex)
            {
                errorf(*logger_, "HDF5Writer [{}] source={} write HDF5 error: {}",
                       config_.name, util::bus::getRootSourceName(entry.batch), ex.getCDetailMsg());
                if (writerMetrics_) writerMetrics_->incrementWriteErrors();
            }
            catch (const std::exception& ex)
            {
                errorf(*logger_, "HDF5Writer [{}] source={} write failed: {}",
                       config_.name, util::bus::getRootSourceName(entry.batch), ex.what());
                if (writerMetrics_) writerMetrics_->incrementWriteErrors();
            }
            catch (...)
            {
                errorf(*logger_, "HDF5Writer [{}] source={} write failed — unknown exception",
                       config_.name, util::bus::getRootSourceName(entry.batch));
                if (writerMetrics_) writerMetrics_->incrementWriteErrors();
            }
        }
    }
    debugf(*logger_, "HDF5Writer [{}] writer thread exited", config_.name);
}

// ---------------------------------------------------------------------------
// flushLoop()
// ---------------------------------------------------------------------------

void HDF5WriterBase::flushLoop()
{
    debugf(*logger_, "HDF5Writer [{}] flush thread started (interval={}ms)", config_.name,
           std::chrono::duration_cast<std::chrono::milliseconds>(config_.flushInterval).count());

    while (!stopping_.load())
    {
        tracef(*logger_, "HDF5Writer [{}] flush thread [tid={}] sleeping {}ms", config_.name,
               std::this_thread::get_id(),
               std::chrono::duration_cast<std::chrono::milliseconds>(config_.flushInterval).count());
        std::this_thread::sleep_for(config_.flushInterval);
        doFlushAll();
    }

    debugf(*logger_, "HDF5Writer [{}] final flush on shutdown", config_.name);
    doFlushAll();
    debugf(*logger_, "HDF5Writer [{}] flush thread exited", config_.name);
}

// ---------------------------------------------------------------------------
// ensureDataset / ensureDataset2D
// ---------------------------------------------------------------------------

H5::DataSet HDF5WriterBase::ensureDataset(H5::H5File&         file,
                                          const std::string&  name,
                                          const H5::DataType& dtype)
{
    if (file.nameExists(name))
        return file.openDataSet(name);

    tracef(*logger_, "HDF5Writer ensureDataset '{}' — creating new chunked dataset (chunk={})", name, kChunkSize);
    hsize_t       dims[1] = {0};
    hsize_t       maxDims[1] = {H5S_UNLIMITED};
    H5::DataSpace space(1, dims, maxDims);

    hsize_t               chunkDims[1] = {kChunkSize};
    H5::DSetCreatPropList props;
    props.setChunk(1, chunkDims);
    if (config_.compressionLevel > 0)
        props.setDeflate(config_.compressionLevel);

    return file.createDataSet(name, dtype, space, props);
}

H5::DataSet HDF5WriterBase::ensureDataset2D(H5::H5File&         file,
                                            const std::string&  name,
                                            const H5::DataType& dtype,
                                            hsize_t             arrayLen)
{
    if (file.nameExists(name))
        return file.openDataSet(name);

    tracef(*logger_, "HDF5Writer ensureDataset2D '{}' — creating new 2D chunked dataset (arrayLen={}, chunk={})", name, arrayLen, kChunkSize);
    hsize_t       dims[2] = {0, arrayLen};
    hsize_t       maxDims[2] = {H5S_UNLIMITED, arrayLen};
    H5::DataSpace space(2, dims, maxDims);

    hsize_t               chunkDims[2] = {kChunkSize, arrayLen};
    H5::DSetCreatPropList props;
    props.setChunk(2, chunkDims);
    if (config_.compressionLevel > 0)
        props.setDeflate(config_.compressionLevel);

    return file.createDataSet(name, dtype, space, props);
}

// ---------------------------------------------------------------------------
// isTabularBatch
// ---------------------------------------------------------------------------

bool HDF5WriterBase::isTabularBatch(const util::bus::IDataBus::EventBatch& batch)
{
    return util::bus::isTimeSeries(batch) && util::bus::asTimeSeries(batch).is_tabular;
}

// ---------------------------------------------------------------------------
// processTabularBatch / accumulateTabularFrame
// ---------------------------------------------------------------------------

void HDF5WriterBase::processTabularBatch(const QueueEntry& entry)
{
    const auto& source = util::bus::asTimeSeries(entry.batch).root_source_name;
    auto&       buf = tabularBuffers_[source];
    // Capture metadata from the first batch that carries non-empty metadata.
    if (buf.pendingMetadata.empty() && !entry.batch.metadata.empty())
        buf.pendingMetadata = entry.batch.metadata;
    for (const auto& frame : util::bus::asTimeSeries(entry.batch).frames)
        accumulateTabularFrame(source, frame, buf);
}

void HDF5WriterBase::accumulateTabularFrame(const std::string&          sourceName,
                                            const util::bus::DataBatch& batch,
                                            TabularBuffer&              buf)
{
    using namespace mldp_pvxs_driver::util::bus;

    int64_t frameFirstTs = -1;
    if (!batch.timestamps.empty())
    {
        const auto& ts0 = batch.timestamps[0];
        frameFirstTs = static_cast<int64_t>(ts0.epoch_seconds) * 1'000'000'000LL +
                       static_cast<int64_t>(ts0.nanoseconds);
    }

    if (buf.rowCount > 0 && frameFirstTs != -1 && frameFirstTs != buf.roundFirstTs)
        flushTabularBufferImpl(sourceName, buf);

    if (buf.rowCount == 0 && !batch.timestamps.empty())
    {
        const std::size_t n = batch.timestamps.size();
        buf.tsSeconds.reserve(n);
        buf.tsNanos.reserve(n);
        for (const auto& ts : batch.timestamps)
        {
            buf.tsSeconds.push_back(static_cast<int64_t>(ts.epoch_seconds));
            buf.tsNanos.push_back(static_cast<int64_t>(ts.nanoseconds));
        }
        buf.rowCount = n;
        buf.roundFirstTs = frameFirstTs;
    }

    for (const auto& col : batch.columns)
    {
        if (col.name.empty())
            continue;

        if (buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
        {
            if (buf.warnedUnknown.size() < TabularBuffer::kMaxWarnedUnknown &&
                buf.warnedUnknown.insert(col.name).second)
            {
                warnf(*logger_,
                      "HDF5Writer tabular source={} unknown column '{}' after schema lock, skipping",
                      sourceName, col.name);
            }
            else if (buf.warnedUnknown.size() == TabularBuffer::kMaxWarnedUnknown &&
                     buf.warnedUnknown.find("__FULL__") == buf.warnedUnknown.end())
            {
                buf.warnedUnknown.insert("__FULL__");
                warnf(*logger_,
                      "HDF5Writer tabular source={} warnedUnknown set full ({} entries), suppressing further warnings",
                      sourceName, TabularBuffer::kMaxWarnedUnknown);
            }
            continue;
        }

        std::visit([&](const auto& vals)
                   {
                       using VecT = std::decay_t<decltype(vals)>;
                       using ElemT = typename VecT::value_type;
                       const std::size_t n = vals.size();
                       if (n == 0)
                           return;

                       if constexpr (std::is_same_v<ElemT, double>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Float64;
                               buf.columns.emplace_back(std::vector<double>{});
                           }
                           auto& vec = std::get<std::vector<double>>(buf.columns[buf.colIndex.at(col.name)]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<double>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, float>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Float32;
                               buf.columns.emplace_back(std::vector<float>{});
                           }
                           auto& vec = std::get<std::vector<float>>(buf.columns[buf.colIndex.at(col.name)]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<float>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, int32_t>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Int32;
                               buf.columns.emplace_back(std::vector<int32_t>{});
                           }
                           auto& vec = std::get<std::vector<int32_t>>(buf.columns[buf.colIndex.at(col.name)]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<int32_t>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, int64_t>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Int64;
                               buf.columns.emplace_back(std::vector<int64_t>{});
                           }
                           auto& vec = std::get<std::vector<int64_t>>(buf.columns[buf.colIndex.at(col.name)]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<int64_t>());
                           for (const auto& v : vals)
                               vec.push_back(v);
                       }
                       else if constexpr (std::is_same_v<ElemT, bool>)
                       {
                           if (!buf.schemaFixed && buf.colIndex.find(col.name) == buf.colIndex.end())
                           {
                               buf.colIndex[col.name] = buf.colNames.size();
                               buf.colNames.push_back(col.name);
                               buf.colTypes[col.name] = FieldType::Bool;
                               buf.columns.emplace_back(std::vector<uint8_t>{});
                           }
                           auto& vec = std::get<std::vector<uint8_t>>(buf.columns[buf.colIndex.at(col.name)]);
                           while (vec.size() + n < buf.rowCount)
                               vec.push_back(fillValue<uint8_t>());
                           for (bool v : vals)
                               vec.push_back(static_cast<uint8_t>(v ? 1 : 0));
                       }
                       else
                       {
                           const std::string key = col.name + ":unsupported_type";
                           if (buf.warnedUnknown.size() < TabularBuffer::kMaxWarnedUnknown &&
                               buf.warnedUnknown.insert(key).second)
                           {
                               warnf(*logger_,
                                     "HDF5Writer tabular source={} column '{}' has unsupported type for tabular path, skipping",
                                     sourceName, col.name);
                           }
                           else if (buf.warnedUnknown.size() == TabularBuffer::kMaxWarnedUnknown &&
                                    buf.warnedUnknown.find("__FULL__") == buf.warnedUnknown.end())
                           {
                               buf.warnedUnknown.insert("__FULL__");
                               warnf(*logger_,
                                     "HDF5Writer tabular source={} warnedUnknown set full ({} entries), suppressing further warnings",
                                     sourceName, TabularBuffer::kMaxWarnedUnknown);
                           }
                       }
                   },
                   col.values);
    }
}

// ---------------------------------------------------------------------------
// flushTabularBuffer — protected helper, used by both subclasses
// ---------------------------------------------------------------------------

void HDF5WriterBase::flushTabularBuffer(const std::string& sourceName,
                                        TabularBuffer&     buf,
                                        H5::H5File&        file)
{
    const std::size_t nRows = buf.rowCount;
    if (nRows == 0)
        return;

    if (!buf.schemaFixed)
    {
        buf.schemaFixed = true;
        infof(*logger_, "HDF5Writer tabular source={} schema locked ({} columns)",
              sourceName, buf.colNames.size());
    }

    const std::size_t nCols = buf.colNames.size();
    if (nCols == 0)
    {
        buf.rowCount = 0;
        return;
    }

    if (!file.nameExists(sourceName))
        file.createGroup(sourceName);

    // Write metadata as string attributes the first time this source group is seen.
    if (seen_groups_.insert(sourceName).second && !buf.pendingMetadata.empty())
    {
        H5::Group           grp = file.openGroup(sourceName);
        const H5::StrType   vlStrType(H5::PredType::C_S1, H5T_VARIABLE);
        const H5::DataSpace scalar(H5S_SCALAR);
        for (const auto& [k, v] : buf.pendingMetadata)
        {
            H5::Attribute attr = grp.createAttribute(k, vlStrType, scalar);
            attr.write(vlStrType, v);
        }
        tracef(*logger_, "HDF5Writer tabular source={} wrote {} metadata attributes",
               sourceName, buf.pendingMetadata.size());
    }

    const std::string secPath = sourceName + "/secondsPastEpoch";
    const std::string nanoPath = sourceName + "/nanoseconds";

    buf.tsSeconds.resize(nRows, 0LL);
    buf.tsNanos.resize(nRows, 0LL);

    {
        H5::DataSet ds = ensureDataset(file, secPath, H5::PredType::NATIVE_INT64);
        append1D(ds, H5::PredType::NATIVE_INT64, buf.tsSeconds.data(), static_cast<hsize_t>(nRows));
    }
    {
        H5::DataSet ds = ensureDataset(file, nanoPath, H5::PredType::NATIVE_INT64);
        append1D(ds, H5::PredType::NATIVE_INT64, buf.tsNanos.data(), static_cast<hsize_t>(nRows));
    }

    for (std::size_t i = 0; i < nCols; ++i)
    {
        const std::string dsPath = sourceName + "/" + buf.colNames[i];
        std::visit([&](auto& vec)
                   {
                       using T = typename std::decay_t<decltype(vec)>::value_type;
                       while (vec.size() < nRows)
                           vec.push_back(fillValue<T>());
                       const H5::PredType& h5t = mapNativeType<T>();
                       H5::DataSet         ds = ensureDataset(file, dsPath, h5t);
                       append1D(ds, h5t, vec.data(), static_cast<hsize_t>(nRows));
                   },
                   buf.columns[i]);
    }

    tracef(*logger_, "HDF5Writer tabular source={} flushed {} rows × {} cols",
           sourceName, nRows, nCols);

    buf.tsSeconds.clear();
    buf.tsNanos.clear();
    buf.rowCount = 0;
    buf.roundFirstTs = -1;
    buf.columns.clear();
    if (buf.schemaFixed)
    {
        buf.columns.resize(nCols);
        for (std::size_t i = 0; i < nCols; ++i)
        {
            switch (buf.colTypes.at(buf.colNames[i]))
            {
            case FieldType::Float64: buf.columns[i] = std::vector<double>{}; break;
            case FieldType::Float32: buf.columns[i] = std::vector<float>{}; break;
            case FieldType::Int32: buf.columns[i] = std::vector<int32_t>{}; break;
            case FieldType::Int64: buf.columns[i] = std::vector<int64_t>{}; break;
            case FieldType::Bool: buf.columns[i] = std::vector<uint8_t>{}; break;
            }
        }
    }
}
