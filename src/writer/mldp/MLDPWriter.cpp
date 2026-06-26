//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/mldp/MLDPWriter.h>

#include <common.pb.h>
#include <util/StringFormat.h>
#include <util/log/Logger.h>

#include <google/protobuf/arena.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;

using mldp_pvxs_driver::util::pool::MLDPGrpcIngestionePool;

namespace {

bool hasTimestampListW(const dp::service::common::DataFrame& frame)
{
    return frame.has_datatimestamps() &&
           frame.datatimestamps().has_timestamplist() &&
           frame.datatimestamps().timestamplist().timestamps_size() > 0;
}

} // namespace

// ---------------------------------------------------------------------------
// StreamState — per-worker gRPC stream lifecycle data
// ---------------------------------------------------------------------------

struct MLDPWriter::StreamState
{
    std::optional<mldp_pvxs_driver::util::pool::PooledHandle<mldp_pvxs_driver::util::pool::MLDPGrpcObject>> handle;
    std::unique_ptr<grpc::ClientWriter<dp::service::ingestion::IngestDataRequest>>                          writer;
    std::unique_ptr<grpc::ClientContext>                                                                    context;
    dp::service::ingestion::IngestDataStreamResponse                                                        response;
    std::chrono::steady_clock::time_point                                                                   streamStart;
    std::size_t   streamPayloadBytes{0};
    std::uint64_t requestCounter{0};
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MLDPWriter::MLDPWriter(const config::Config&    root,
                       std::shared_ptr<Metrics> metrics)
    : MLDPWriter(MLDPWriterConfig::parse(root), std::move(metrics))
{
}

MLDPWriter::MLDPWriter(MLDPWriterConfig         config,
                       std::shared_ptr<Metrics> metrics)
    : config_(std::move(config))
    , logger_(mldp_pvxs_driver::util::log::newLogger("grpc_writer:" + config_.name))
    , metrics_(std::move(metrics))
{
}

MLDPWriter::~MLDPWriter()
{
    if (running_.load())
    {
        stop();
    }
    threadPool_.reset();
    ingestionPool_.reset();
}

// ---------------------------------------------------------------------------
// IWriter lifecycle
// ---------------------------------------------------------------------------

void MLDPWriter::start()
{
    if (running_.load())
    {
        warnf(*logger_, "MLDPWriter already started");
        return;
    }

    running_.store(true);
    infof(*logger_, "MLDPWriter starting");

    threadPool_ = std::make_shared<BS::light_thread_pool>(
        static_cast<std::size_t>(std::max(1, config_.threadPoolSize)),
        [](std::size_t i)
        {
            BS::this_thread::set_os_thread_name("mldp-pool-" + std::to_string(i));
        });

    ingestionPool_ = MLDPGrpcIngestionePool::create(config_.poolConfig, metrics_);
    providerId_ = ingestionPool_->providerId();
    if (providerId_.empty())
    {
        running_.store(false);
        throw std::runtime_error("MLDPWriter: failed to register provider with MLDP ingestion service");
    }

    const std::size_t workerCount = std::max<std::size_t>(
        1, static_cast<std::size_t>(config_.threadPoolSize));
    nextChannel_.store(0, std::memory_order_relaxed);
    queuedItems_.store(0);
    channels_.clear();
    channels_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
    {
        channels_.push_back(std::make_unique<WorkerChannel>());
    }
    for (std::size_t i = 0; i < workerCount; ++i)
    {
        threadPool_->detach_task([this, i]()
                                 {
                                     workerLoop(i);
                                 });
    }

    infof(*logger_, "MLDPWriter started with {} workers", workerCount);
}

void MLDPWriter::stop() noexcept
{
    if (!running_.load())
    {
        return;
    }

    const auto pending = queuedItems_.load(std::memory_order_relaxed);
    infof(*logger_, "MLDPWriter stopping — {} item(s) pending in queue", pending);

    running_.store(false);
    for (auto& ch : channels_)
    {
        {
            std::lock_guard lk(ch->mutex);
            ch->shutdown = true;
        }
        ch->cv.notify_one();
    }

    debugf(*logger_, "MLDPWriter waiting for worker threads to drain...");
    if (threadPool_)
    {
        threadPool_->wait();
    }

    channels_.clear();
    infof(*logger_, "MLDPWriter stopped — all queues drained");
}

bool MLDPWriter::isHealthy() const noexcept
{
    return running_.load();
}

const std::string& MLDPWriter::providerId() const
{
    return providerId_;
}

// ---------------------------------------------------------------------------
// push — round-robin across worker channels
// ---------------------------------------------------------------------------

bool MLDPWriter::push(util::bus::IDataBus::EventBatch batch) noexcept
{
    if (!running_.load())
    {
        return false;
    }
    if (!util::bus::isTimeSeries(batch))
    {
        return true;
    }
    auto& ts_mut = std::get<util::bus::TimeSeriesPayload>(batch.payload);
    if (ts_mut.end_of_batch_group)
    {
        // Marker-only batch emitted by readers to signal end of one NTTable
        // update round.  Nothing to forward to gRPC — skip silently.
        return true;
    }
    const std::string rootSourceName = ts_mut.root_source_name;
    if (rootSourceName.empty() || ts_mut.frames.empty())
    {
        return false;
    }

    auto metadata = std::make_shared<const std::unordered_map<std::string, std::string>>(batch.metadata);
    bool enqueued = false;
    for (auto& frame : ts_mut.frames)
    {
        if (frame.timestamps.empty())
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", rootSourceName}});
                        });
            continue;
        }
        const auto idx = nextChannel_.fetch_add(1, std::memory_order_relaxed) % channels_.size();
        QueueItem  item{rootSourceName, metadata, std::move(frame)};
        {
            std::lock_guard lk(channels_[idx]->mutex);
            channels_[idx]->items.push_back(std::move(item));
        }
        channels_[idx]->cv.notify_one();
        queuedItems_.fetch_add(1, std::memory_order_relaxed);
        enqueued = true;
    }
    updateQueueDepthMetric();
    return enqueued;
}

// ---------------------------------------------------------------------------
// closeStream — finish and tear down the active gRPC stream
// ---------------------------------------------------------------------------

void MLDPWriter::closeStream(StreamState& state, const char* reason) noexcept
{
    if (!state.writer)
    {
        return;
    }
    state.writer->WritesDone();
    auto    status = state.writer->Finish();
    int64_t requestedRequests = static_cast<int64_t>(state.requestCounter);
    if (status.ok())
    {
        if (state.response.has_ingestdatastreamresult())
        {
            const auto& result = state.response.ingestdatastreamresult();
            if (result.numrequests() < 0)
            {
                errorf(*logger_, "Ingestion stream finished with invalid numrequests ({}): {}", reason, result.numrequests());
            }
            else if (result.numrequests() < requestedRequests)
            {
                errorf(*logger_, "Ingestion stream finished with incomplete requests ({}): server accepted {} of {} sent",
                       reason, result.numrequests(), requestedRequests);
            }
            else if (result.numrequests() > requestedRequests)
            {
                errorf(*logger_, "Ingestion stream finished with mismatch ({}): server reports {} but we sent {}",
                       reason, result.numrequests(), requestedRequests);
            }
            else
            {
                tracef(*logger_, "Ingestion stream finished successfully ({}): {} requests", reason, result.numrequests());
            }
        }
        if (state.response.has_exceptionalresult())
        {
            errorf(*logger_, "Ingestion stream finished with exceptional result ({}): {}",
                   reason, state.response.exceptionalresult().message());
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}});
                        });
        }
    }
    else
    {
        errorf(*logger_, "Ingestion stream finished with error ({}): {}", reason, status.error_message());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}});
                    });
    }
    metric_call(metrics_, [&](auto& m)
                {
                    m.incrementWriterStreamRotations(1.0, {{"writer", config_.name}, {"reason", reason}});
                });
    state.writer.reset();
    state.handle.reset();
    state.context.reset();
    state.streamPayloadBytes = 0;
    state.requestCounter = 0;
}

// ---------------------------------------------------------------------------
// ensureStream — open a new gRPC stream if none is active
// ---------------------------------------------------------------------------

bool MLDPWriter::ensureStream(StreamState& state)
{
    if (state.writer)
    {
        return true;
    }
    try
    {
        state.context  = std::make_unique<grpc::ClientContext>();
        state.response = dp::service::ingestion::IngestDataStreamResponse();
        state.handle.emplace(ingestionPool_->acquire());
        state.writer = (*state.handle)->stub->ingestDataStream(state.context.get(), &state.response);
        if (!state.writer)
        {
            errorf(*logger_, "Failed to open ingestion stream");
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}});
                        });
            state.handle.reset();
            state.context.reset();
            return false;
        }
        state.streamStart        = std::chrono::steady_clock::now();
        state.streamPayloadBytes = 0;
        return true;
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_, "Failed to acquire ingestion stream: {}", ex.what());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}});
                    });
        state.handle.reset();
        state.writer.reset();
        return false;
    }
}

// ---------------------------------------------------------------------------
// workerLoop — per-worker gRPC ingestion loop
//
// INPUT  (QueueItem dequeued from WorkerChannel)
//   root_source   EPICS PV name; written as provenance.source on every column
//   metadata      shared KV map; appended as attributes on every column
//   frame         DataBatch
//     .timestamps[]    EpicsTimestamp { epoch_seconds, nanoseconds }
//     .columns[]       Column { name, values: variant<scalar | waveform> }
//     .enum_columns[]  EnumColumn { name, enum_id, values[] }
//     .array_dims      map<col_name, ArrayDims{ dims[] }>
//
// STAGE 1 — toDataFrame()   DataBatch → dp::service::common::DataFrame
//
//   Source field                        DataFrame proto field
//   ─────────────────────────────────────────────────────────────────────
//   timestamps[i].epoch_seconds    →    DataTimestamps
//   timestamps[i].nanoseconds           .TimestampList.timestamps[i]
//                                       .{epochseconds, nanoseconds}
//
//   Column.values variant → typed repeated field (one proto column per Column):
//     vector<double>    →  doublecolumns[k].values[]
//     vector<float>     →  floatcolumns[k].values[]
//     vector<int64_t>   →  int64columns[k].values[]
//     vector<int32_t>   →  int32columns[k].values[]
//     vector<bool>      →  boolcolumns[k].values[]
//     vector<string>    →  stringcolumns[k].values[]
//     vector<uint8_t[]> →  structcolumns[k].values[]          (opaque blob)
//     vector<double[]>  →  doublearraycolumns[k].values[]     ┐ waveform:
//     vector<float[]>   →  floatarraycolumns[k].values[]      │ N rows × M elems,
//     vector<int64_t[]> →  int64arraycolumns[k].values[]      │ flattened
//     vector<int32_t[]> →  int32arraycolumns[k].values[]      │ row-major into
//     vector<bool[]>    →  boolarraycolumns[k].values[]       ┘ one repeated field
//
//   array_dims[col].dims[]         →  *arraycolumns[k].dimensions.dims[]
//   EnumColumn.{enum_id,values[]}  →  enumcolumns[k].{enumid, values[]}
//   root_source                    →  column[k].metadata.provenance.source
//   metadata KV                    →  column[k].metadata.attributes[]
//
// STAGE 2 — buildRequest()   DataFrame → dp::service::ingestion::IngestDataRequest
//
//   .providerid        = providerId_   (registered at start())
//   .clientrequestid   = "pv_stream_<streamEpoch>_<source>_<seqno>"
//   .ingestiondataframe = DataFrame from Stage 1
//
//   Drop guards (frame skipped on failure):
//     no typed columns present  → warnf, return false
//     TimestampList empty       → errorf + writer_failures++, return false
//
//   Outputs: acceptedEvents = timestamps_size()
//            payloadBytes   = ByteSizeLong()   (serialised wire size)
//
// STAGE 3 — state.writer->Write(request)   send over open gRPC stream
//
//   On Write() == false: writer_failures++, rotate stream, skip item.
//
//   Counters  (monotonic, labelled by writer + source)
//     writer_pushes            += acceptedEvents
//     writer_payload_bytes     += payloadBytes    (proto wire size)
//     writer_data_bytes_total  += dataBatchBytes  (C++ in-memory size)
//
//   Instant-rate gauges  (skipped when Δt = 0, i.e. same EPICS pulse)
//     data_bytes_per_second           = dataBatchBytes / Δt
//     payload_bytes_per_second        = payloadBytes   / Δt
//     post_conv_data_bytes_per_second = payloadBytes   / Δt
//
//   Δt = gap between last two distinct EPICS pulse timestamps for root_source
//        (tracked in lastEventTime_[source]).
//
// STREAM LIFECYCLE — ensureStream / closeStream
//
//   One gRPC ClientWriter<IngestDataRequest> is reused across many requests.
//   Rotation (closeStream → ensureStream) when:
//     1. stream age   ≥ streamMaxAge    pre-write age check
//     2. cumul bytes  ≥ streamMaxBytes  pre-write byte check
//     3. Write() returns false          write failure
//     4. idle timeout (empty queue)     post-wait age check
//     5. age or bytes met               post-write check
//   closeStream: WritesDone(), drain server response, log request-count
//                mismatch, zero all StreamState fields.
//   Idle:     rotate if stale, loop back to wait.
//   Shutdown: drain remaining items, close("shutdown"), return.
// ---------------------------------------------------------------------------

void MLDPWriter::workerLoop(std::size_t workerIndex)
{
    auto&       ch    = *channels_[workerIndex];
    StreamState state;
    std::size_t drainCount    = 0;
    auto        lastDrainLog  = std::chrono::steady_clock::time_point{};

    const auto dequeueTimeout = config_.streamMaxAge;
    while (true)
    {
        QueueItem item;
        bool      hasItem = false;
        {
            std::unique_lock lk(ch.mutex);
            ch.cv.wait_for(lk, dequeueTimeout, [&]
                           {
                               return !ch.items.empty() || ch.shutdown;
                           });
            if (ch.shutdown && ch.items.empty())
            {
                break;
            }
            if (!ch.items.empty())
            {
                item    = std::move(ch.items.front());
                hasItem = true;
                ch.items.pop_front();
            }
        }

        if (!hasItem)
        {
            if (state.writer)
            {
                const auto elapsed = std::chrono::steady_clock::now() - state.streamStart;
                if (elapsed >= config_.streamMaxAge)
                {
                    closeStream(state, "stream age exceeded (idle)");
                }
            }
            continue;
        }

        queuedItems_.fetch_sub(1, std::memory_order_relaxed);
        updateQueueDepthMetric();

        if (!running_.load())
        {
            ++drainCount;
            const auto now = std::chrono::steady_clock::now();
            if (drainCount == 1 || now - lastDrainLog >= std::chrono::seconds(10))
            {
                infof(*logger_, "MLDPWriter worker[{}] draining — processed {} item(s), {} remaining",
                      workerIndex, drainCount, queuedItems_.load(std::memory_order_relaxed));
                lastDrainLog = now;
            }
        }

        const auto itemStart        = std::chrono::steady_clock::now();
        const auto record_send_time = [this, itemStart](prometheus::Labels tags)
        {
            const auto   elapsed = std::chrono::steady_clock::now() - itemStart;
            const double sec     = std::chrono::duration<double>(elapsed).count();
            metric_call(metrics_, [&](auto& m)
                        {
                            m.observeControllerSendTimeSeconds(sec, std::move(tags));
                        });
        };

        if (!ensureStream(state))
        {
            record_send_time({{"source", "unknown"}});
            continue;
        }

        if (std::chrono::steady_clock::now() - state.streamStart >= config_.streamMaxAge)
        {
            closeStream(state, "stream age exceeded");
            if (!ensureStream(state))
            {
                record_send_time({{"source", "unknown"}});
                continue;
            }
        }

        google::protobuf::Arena arena;
        auto* request = google::protobuf::Arena::CreateMessage<
            dp::service::ingestion::IngestDataRequest>(&arena);
        std::size_t       acceptedEvents = 0;
        std::size_t       payloadBytes   = 0;
        const std::size_t dataBatchBytes = util::bus::estimateDataBatchBytes(item.frame);
        const auto        requestId      = mldp_pvxs_driver::util::format_string(
            "pv_stream_{}_{}_{}", state.streamStart.time_since_epoch().count(),
            item.root_source, state.requestCounter);

        if (!buildRequest(item.root_source, item.frame, requestId,
                          *request, acceptedEvents, payloadBytes,
                          item.metadata.get()))
        {
            continue;
        }

        if ((state.streamPayloadBytes + payloadBytes) > config_.streamMaxBytes &&
            state.streamPayloadBytes > 0)
        {
            closeStream(state, "max bytes exceeded");
            if (!ensureStream(state))
            {
                record_send_time({{"source", "unknown"}});
                continue;
            }
        }

        if (!state.writer->Write(*request))
        {
            errorf(*logger_, "Failed to write source {} to ingestion stream", item.root_source);
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", item.root_source}});
                        });
            closeStream(state, "write failed");
            continue;
        }

        ++state.requestCounter;
        state.streamPayloadBytes += payloadBytes;
        if (acceptedEvents > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterPushes(static_cast<double>(acceptedEvents),
                                                    {{"writer", config_.name}, {"source", item.root_source}});
                        });
        }
        if (payloadBytes > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterPayloadBytes(static_cast<double>(payloadBytes),
                                                          {{"writer", config_.name}, {"source", item.root_source}});
                        });
        }
        if (dataBatchBytes > 0)
        {
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterDataBytesTotal(static_cast<double>(dataBatchBytes),
                                                            {{"source", item.root_source}});
                        });
        }

        if (!item.frame.timestamps.empty())
        {
            const auto&  ts        = item.frame.timestamps.back();
            const double event_sec = static_cast<double>(ts.epoch_seconds)
                                     + static_cast<double>(ts.nanoseconds) * 1e-9;
            std::lock_guard<std::mutex> lk(lastEventTimeMutex_);
            auto& prev = lastEventTime_[item.root_source];
            if (prev > 0.0)
            {
                const double delta_sec = event_sec - prev;
                if (delta_sec > 0.0)  // new pulse only; same-pulse columns → skip
                {
                    if (dataBatchBytes > 0)
                    {
                        metric_call(metrics_, [&](auto& m)
                                    {
                                        m.setWriterDataBytesPerSecond(
                                            static_cast<double>(dataBatchBytes) / delta_sec,
                                            {{"source", item.root_source}});
                                    });
                    }
                    if (payloadBytes > 0)
                    {
                        const double payload_bps = static_cast<double>(payloadBytes) / delta_sec;
                        metric_call(metrics_, [&](auto& m)
                                    {
                                        m.setWriterPayloadBytesPerSecond(
                                            payload_bps,
                                            {{"writer", config_.name}, {"source", item.root_source}});
                                    });
                        metric_call(metrics_, [&](auto& m)
                                    {
                                        m.setWriterPostConvDataBytesPerSecond(
                                            payload_bps,
                                            {{"writer", config_.name}, {"source", item.root_source}});
                                    });
                    }
                }
            }
            if (prev <= 0.0 || event_sec > prev)
            {
                prev = event_sec;
            }
        }
        record_send_time({{"source", item.root_source}});

        const auto postElapsed = std::chrono::steady_clock::now() - state.streamStart;
        if (postElapsed >= config_.streamMaxAge || state.streamPayloadBytes >= config_.streamMaxBytes)
        {
            closeStream(state, "threshold reached");
        }
    }

    if (drainCount > 0)
    {
        infof(*logger_, "MLDPWriter worker[{}] drain complete — processed {} item(s)",
              workerIndex, drainCount);
    }
    closeStream(state, "shutdown");
}

// ---------------------------------------------------------------------------
// toDataFrame — convert DataBatch → protobuf DataFrame
// ---------------------------------------------------------------------------

dp::service::common::DataFrame MLDPWriter::toDataFrame(const util::bus::DataBatch&                         batch,
                                                       const std::string&                                  rootSource,
                                                       const std::unordered_map<std::string, std::string>* metadata)
{
    dp::service::common::DataFrame df;

    const auto apply_metadata = [&](auto* c)
    {
        if (metadata)
        {
            for (const auto& [k, v] : *metadata)
            {
                auto* attr = c->mutable_metadata()->add_attributes();
                attr->set_name(k);
                attr->set_value(v);
            }
        }
    };

    // Timestamps
    {
        auto* tsList = df.mutable_datatimestamps()->mutable_timestamplist();
        for (const auto& ts : batch.timestamps)
        {
            auto* entry = tsList->add_timestamps();
            entry->set_epochseconds(static_cast<uint64_t>(ts.epoch_seconds));
            entry->set_nanoseconds(static_cast<uint64_t>(ts.nanoseconds));
        }
    }

    // Typed columns
    for (const auto& col : batch.columns)
    {
        std::visit(
            [&](const auto& vec)
            {
                using T = std::decay_t<decltype(vec)>;

                if constexpr (std::is_same_v<T, std::vector<double>>)
                {
                    auto* c = df.add_doublecolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (auto v : vec)
                    {
                        c->add_values(v);
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    auto* c = df.add_floatcolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (auto v : vec)
                    {
                        c->add_values(v);
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<int64_t>>)
                {
                    auto* c = df.add_int64columns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (auto v : vec)
                    {
                        c->add_values(v);
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<int32_t>>)
                {
                    auto* c = df.add_int32columns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (auto v : vec)
                    {
                        c->add_values(v);
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<bool>>)
                {
                    auto* c = df.add_boolcolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (auto v : vec)
                    {
                        c->add_values(v);
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                {
                    auto* c = df.add_stringcolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (const auto& v : vec)
                    {
                        c->add_values(v);
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<uint8_t>>>)
                {
                    auto* c = df.add_structcolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    c->set_schemaid("");
                    for (const auto& blob : vec)
                    {
                        c->add_values(blob.data(), blob.size());
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<double>>>)
                {
                    auto* c = df.add_doublearraycolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (const auto& arr : vec)
                    {
                        for (auto v : arr)
                        {
                            c->add_values(v);
                        }
                    }
                    auto it = batch.array_dims.find(col.name);
                    if (it != batch.array_dims.end())
                    {
                        for (auto d : it->second.dims)
                        {
                            c->mutable_dimensions()->add_dims(d);
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<float>>>)
                {
                    auto* c = df.add_floatarraycolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (const auto& arr : vec)
                    {
                        for (auto v : arr)
                        {
                            c->add_values(v);
                        }
                    }
                    auto it = batch.array_dims.find(col.name);
                    if (it != batch.array_dims.end())
                    {
                        for (auto d : it->second.dims)
                        {
                            c->mutable_dimensions()->add_dims(d);
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<int64_t>>>)
                {
                    auto* c = df.add_int64arraycolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (const auto& arr : vec)
                    {
                        for (auto v : arr)
                        {
                            c->add_values(v);
                        }
                    }
                    auto it = batch.array_dims.find(col.name);
                    if (it != batch.array_dims.end())
                    {
                        for (auto d : it->second.dims)
                        {
                            c->mutable_dimensions()->add_dims(d);
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<int32_t>>>)
                {
                    auto* c = df.add_int32arraycolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (const auto& arr : vec)
                    {
                        for (auto v : arr)
                        {
                            c->add_values(v);
                        }
                    }
                    auto it = batch.array_dims.find(col.name);
                    if (it != batch.array_dims.end())
                    {
                        for (auto d : it->second.dims)
                        {
                            c->mutable_dimensions()->add_dims(d);
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<bool>>>)
                {
                    auto* c = df.add_boolarraycolumns();
                    c->set_name(col.name);
                    c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
                    apply_metadata(c);
                    for (const auto& arr : vec)
                    {
                        for (auto v : arr)
                        {
                            c->add_values(v);
                        }
                    }
                    auto it = batch.array_dims.find(col.name);
                    if (it != batch.array_dims.end())
                    {
                        for (auto d : it->second.dims)
                        {
                            c->mutable_dimensions()->add_dims(d);
                        }
                    }
                }
            },
            col.values);
    }

    // Enum columns
    for (const auto& ecol : batch.enum_columns)
    {
        auto* c = df.add_enumcolumns();
        c->set_name(ecol.name);
        c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
        apply_metadata(c);
        c->set_enumid(ecol.enum_id);
        for (auto v : ecol.values)
        {
            c->add_values(v);
        }
    }

    return df;
}

// ---------------------------------------------------------------------------
// buildRequest — convert DataBatch → IngestDataRequest
// ---------------------------------------------------------------------------

bool MLDPWriter::buildRequest(const std::string&                                  sourceName,
                              const util::bus::DataBatch&                         batch,
                              const std::string&                                  requestId,
                              dp::service::ingestion::IngestDataRequest&          request,
                              std::size_t&                                        acceptedEvents,
                              std::size_t&                                        payloadBytes,
                              const std::unordered_map<std::string, std::string>* metadata)
{
    request.set_providerid(providerId_);
    request.set_clientrequestid(requestId);

    auto  dataFrame     = toDataFrame(batch, sourceName, metadata);
    auto* dataFrame_ptr = request.mutable_ingestiondataframe();
    *dataFrame_ptr      = std::move(dataFrame);

    const bool hasColumns =
        dataFrame_ptr->doublecolumns_size() > 0 || dataFrame_ptr->floatcolumns_size() > 0 ||
        dataFrame_ptr->datacolumns_size() > 0 || dataFrame_ptr->int32columns_size() > 0 ||
        dataFrame_ptr->int64columns_size() > 0 || dataFrame_ptr->boolcolumns_size() > 0 ||
        dataFrame_ptr->stringcolumns_size() > 0 || dataFrame_ptr->enumcolumns_size() > 0 ||
        dataFrame_ptr->imagecolumns_size() > 0 || dataFrame_ptr->structcolumns_size() > 0 ||
        dataFrame_ptr->doublearraycolumns_size() > 0 || dataFrame_ptr->floatarraycolumns_size() > 0 ||
        dataFrame_ptr->int32arraycolumns_size() > 0 || dataFrame_ptr->int64arraycolumns_size() > 0 ||
        dataFrame_ptr->boolarraycolumns_size() > 0;

    if (!hasColumns)
    {
        warnf(*logger_, "No valid columns for source {}, skipping request", sourceName);
        return false;
    }

    if (!hasTimestampListW(*dataFrame_ptr))
    {
        errorf(*logger_, "Dropping frame for source {}: missing DataFrame.datatimestamps.timestamplist", sourceName);
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", sourceName}});
                    });
        return false;
    }

    acceptedEvents = static_cast<std::size_t>(
        dataFrame_ptr->datatimestamps().timestamplist().timestamps_size());
    payloadBytes = static_cast<std::size_t>(request.ByteSizeLong());
    return true;
}

// ---------------------------------------------------------------------------
// Metrics helper
// ---------------------------------------------------------------------------

void MLDPWriter::updateQueueDepthMetric()
{
    const double depth = static_cast<double>(queuedItems_.load(std::memory_order_relaxed));
    metric_call(metrics_, [&](auto& m)
                {
                    m.setControllerQueueDepth(depth);
                });
}
