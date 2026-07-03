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

#include <chrono>
#include <cstring>
#include <cstdint>
#include <optional>
#include <stdexcept>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::metrics;

using mldp_pvxs_driver::util::pool::MLDPGrpcIngestionePool;

namespace {

constexpr std::string_view kUnknownSource = "unknown";

bool hasTimestampList(const dp::service::common::DataFrame& frame)
{
    return frame.has_datatimestamps() &&
           frame.datatimestamps().has_timestamplist() &&
           frame.datatimestamps().timestamplist().timestamps_size() > 0;
}

bool hasAnyColumn(const dp::service::common::DataFrame& f)
{
    return f.doublecolumns_size()      > 0 || f.floatcolumns_size()       > 0 ||
           f.datacolumns_size()        > 0 || f.int32columns_size()        > 0 ||
           f.int64columns_size()       > 0 || f.boolcolumns_size()         > 0 ||
           f.stringcolumns_size()      > 0 || f.enumcolumns_size()         > 0 ||
           f.imagecolumns_size()       > 0 || f.structcolumns_size()       > 0 ||
           f.doublearraycolumns_size() > 0 || f.floatarraycolumns_size()   > 0 ||
           f.int32arraycolumns_size()  > 0 || f.int64arraycolumns_size()   > 0 ||
           f.boolarraycolumns_size()   > 0;
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

    google::protobuf::Arena arena;

    std::unordered_map<std::string, MLDPWriter::SourceRateTracker> rateTrackers;
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
    : BaseQueuedWriter<QueueItem>(
          QueueConfig{static_cast<int>(config.queueCapacity), config.threadPoolSize, 0,
                      static_cast<int>(config.streamMaxAge.count())},
          "grpc_writer:" + config.name,
          mldp_pvxs_driver::util::log::newLogger("grpc_writer:" + config.name))
    , config_(std::move(config))
    , metrics_(std::move(metrics))
{
}

MLDPWriter::~MLDPWriter()
{
    stop();
}

// ---------------------------------------------------------------------------
// IWriter lifecycle hooks
// ---------------------------------------------------------------------------

void MLDPWriter::doStart()
{
    ingestionPool_ = MLDPGrpcIngestionePool::create(config_.poolConfig, metrics_);
    providerId_    = ingestionPool_->providerId();
    if (providerId_.empty())
    {
        ingestionPool_.reset();
        throw std::runtime_error("MLDPWriter: failed to register provider with MLDP ingestion service");
    }
    const auto wc = static_cast<std::size_t>(workerCount());
    workerStates_.clear();
    workerStates_.reserve(wc);
    for (std::size_t i = 0; i < wc; ++i)
        workerStates_.push_back(std::make_unique<StreamState>());
    infof(logger(), "MLDPWriter provider registered: {}", providerId_);
}

void MLDPWriter::doStop() noexcept
{
    for (auto& statePtr : workerStates_)
        if (statePtr) closeStream(*statePtr, "writer stopping");
    workerStates_.clear();
    ingestionPool_.reset();
}

const std::string& MLDPWriter::providerId() const
{
    return providerId_;
}

// ---------------------------------------------------------------------------
// toItems — convert EventBatch to per-frame QueueItems
// ---------------------------------------------------------------------------

std::vector<MLDPWriter::QueueItem> MLDPWriter::toItems(util::bus::IDataBus::EventBatch& batch)
{
    if (!util::bus::isTimeSeries(batch))
        return {};

    auto& ts_mut = std::get<util::bus::TimeSeriesPayload>(batch.payload);
    if (ts_mut.end_of_batch_group)
        return {};

    const std::string rootSourceName = ts_mut.root_source_name;
    if (rootSourceName.empty() || ts_mut.frames.empty())
        return {};

    auto metadata = std::make_shared<const std::unordered_map<std::string, std::string>>(batch.metadata);

    std::vector<QueueItem> items;
    items.reserve(ts_mut.frames.size());
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
        items.push_back({rootSourceName, metadata, std::move(frame)});
    }

    if (!items.empty())
    {
        updateQueueDepthMetric();
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lk(pushLogMutex_);
        if (now - lastPushLogTime_ >= std::chrono::seconds(10))
        {
            lastPushLogTime_ = now;
            infof(logger(), "Pushed {} element(s) for source '{}', queue depth: {}",
                  items.size(), rootSourceName, queueDepth());
        }
    }
    return items;
}

// ---------------------------------------------------------------------------
// processItem — per-worker gRPC send
// ---------------------------------------------------------------------------

void MLDPWriter::processItem(std::size_t workerIndex, QueueItem item)
{
    auto& state = *workerStates_[workerIndex];

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
        record_send_time({{"source", std::string(kUnknownSource)}});
        return;
    }

    if (std::chrono::steady_clock::now() - state.streamStart >= config_.streamMaxAge)
    {
        if (!rotateStream(state, "stream age exceeded"))
        {
            record_send_time({{"source", std::string(kUnknownSource)}});
            return;
        }
    }

    state.arena.Reset();
    auto* request = google::protobuf::Arena::CreateMessage<
        dp::service::ingestion::IngestDataRequest>(&state.arena);
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
        return;
    }

    if ((state.streamPayloadBytes + payloadBytes) > config_.streamMaxBytes &&
        state.streamPayloadBytes > 0)
    {
        if (!rotateStream(state, "max bytes exceeded"))
        {
            record_send_time({{"source", std::string(kUnknownSource)}});
            return;
        }
    }

    if (!state.writer->Write(*request))
    {
        errorf(logger(), "Failed to write source {} to ingestion stream", item.root_source);
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", item.root_source}});
                    });
        closeStream(state, "write failed");
        return;
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

    updateSourceRateMetrics(state, item.root_source, dataBatchBytes, payloadBytes);
    record_send_time({{"source", item.root_source}});

    const auto postElapsed = std::chrono::steady_clock::now() - state.streamStart;
    if (postElapsed >= config_.streamMaxAge || state.streamPayloadBytes >= config_.streamMaxBytes)
        closeStream(state, "threshold reached");
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
    int64_t sentRequests = static_cast<int64_t>(state.requestCounter);
    if (status.ok())
    {
        if (state.response.has_ingestdatastreamresult())
        {
            const auto& result = state.response.ingestdatastreamresult();
            if (result.numrequests() < 0)
            {
                errorf(logger(), "Ingestion stream finished with invalid numrequests ({}): {}", reason, result.numrequests());
            }
            else if (result.numrequests() < sentRequests)
            {
                errorf(logger(), "Ingestion stream finished with incomplete requests ({}): server accepted {} of {} sent",
                       reason, result.numrequests(), sentRequests);
            }
            else if (result.numrequests() > sentRequests)
            {
                errorf(logger(), "Ingestion stream finished with mismatch ({}): server reports {} but we sent {}",
                       reason, result.numrequests(), sentRequests);
            }
            else
            {
                tracef(logger(), "Ingestion stream finished successfully ({}): {} requests", reason, result.numrequests());
            }
        }
        if (state.response.has_exceptionalresult())
        {
            errorf(logger(), "Ingestion stream finished with exceptional result ({}): {}",
                   reason, state.response.exceptionalresult().message());
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", std::string(kUnknownSource)}});
                        });
        }
    }
    else
    {
        errorf(logger(), "Ingestion stream finished with error ({}): {}", reason, status.error_message());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", std::string(kUnknownSource)}});
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
            errorf(logger(), "Failed to open ingestion stream");
            metric_call(metrics_, [&](auto& m)
                        {
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", std::string(kUnknownSource)}});
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
        errorf(logger(), "Failed to acquire ingestion stream: {}", ex.what());
        metric_call(metrics_, [&](auto& m)
                    {
                        m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", std::string(kUnknownSource)}});
                    });
        state.handle.reset();
        state.writer.reset();
        return false;
    }
}

// ---------------------------------------------------------------------------
// rotateStream — close then immediately re-open a gRPC stream
// ---------------------------------------------------------------------------

bool MLDPWriter::rotateStream(StreamState& state, const char* reason)
{
    closeStream(state, reason);
    return ensureStream(state);
}

// ---------------------------------------------------------------------------
// onWorkerIdle — close stale streams when no items arrive within idle_check_ms
// ---------------------------------------------------------------------------

void MLDPWriter::onWorkerIdle(std::size_t workerIndex)
{
    auto& state = *workerStates_[workerIndex];
    if (state.writer &&
        std::chrono::steady_clock::now() - state.streamStart >= config_.streamMaxAge)
    {
        closeStream(state, "stream age exceeded (idle)");
    }
}

// ---------------------------------------------------------------------------
// updateSourceRateMetrics — windowed throughput tracking per source
// ---------------------------------------------------------------------------

void MLDPWriter::updateSourceRateMetrics(StreamState&       state,
                                         const std::string& source,
                                         std::size_t        dataBatchBytes,
                                         std::size_t        payloadBytes)
{
    auto&      tracker = state.rateTrackers[source];
    const auto now     = std::chrono::steady_clock::now();

    tracker.accumulatedBytes        += dataBatchBytes;
    tracker.accumulatedPayloadBytes += payloadBytes;

    if (tracker.lastWallTime.time_since_epoch().count() == 0)
    {
        tracker.lastWallTime = now;
        return;
    }

    const double wall_delta = std::chrono::duration<double>(now - tracker.lastWallTime).count();
    if (wall_delta < 1.0)
        return;

    if (tracker.accumulatedBytes > 0)
    {
        metric_call(metrics_, [&](auto& m)
                    {
                        m.setWriterDataBytesPerSecond(
                            static_cast<double>(tracker.accumulatedBytes) / wall_delta,
                            {{"source", source}});
                    });
    }
    if (tracker.accumulatedPayloadBytes > 0)
    {
        const double payload_bps = static_cast<double>(tracker.accumulatedPayloadBytes) / wall_delta;
        metric_call(metrics_, [&](auto& m)
                    {
                        m.setWriterPayloadBytesPerSecond(
                            payload_bps,
                            {{"writer", config_.name}, {"source", source}});
                    });
        metric_call(metrics_, [&](auto& m)
                    {
                        m.setWriterPostConvDataBytesPerSecond(
                            payload_bps,
                            {{"writer", config_.name}, {"source", source}});
                    });
    }
    tracker.accumulatedBytes        = 0;
    tracker.accumulatedPayloadBytes = 0;
    tracker.lastWallTime            = now;
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

    const auto setup_col = [&](auto* c, const std::string& name)
    {
        c->set_name(name);
        c->mutable_metadata()->mutable_provenance()->set_source(rootSource);
        apply_metadata(c);
    };

    const auto apply_dims = [&](auto* c, const std::string& colName)
    {
        if (auto it = batch.array_dims.find(colName); it != batch.array_dims.end())
            for (auto d : it->second.dims)
                c->mutable_dimensions()->add_dims(d);
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
                    setup_col(c, col.name);
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(vec.size()), 0.0);
                    std::memcpy(vals->mutable_data(), vec.data(), vec.size() * sizeof(double));
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    auto* c = df.add_floatcolumns();
                    setup_col(c, col.name);
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(vec.size()), 0.0f);
                    std::memcpy(vals->mutable_data(), vec.data(), vec.size() * sizeof(float));
                }
                else if constexpr (std::is_same_v<T, std::vector<int64_t>>)
                {
                    auto* c = df.add_int64columns();
                    setup_col(c, col.name);
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(vec.size()), 0);
                    std::memcpy(vals->mutable_data(), vec.data(), vec.size() * sizeof(int64_t));
                }
                else if constexpr (std::is_same_v<T, std::vector<int32_t>>)
                {
                    auto* c = df.add_int32columns();
                    setup_col(c, col.name);
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(vec.size()), 0);
                    std::memcpy(vals->mutable_data(), vec.data(), vec.size() * sizeof(int32_t));
                }
                else if constexpr (std::is_same_v<T, std::vector<bool>>)
                {
                    auto* c = df.add_boolcolumns();
                    setup_col(c, col.name);
                    c->mutable_values()->Reserve(static_cast<int>(vec.size()));
                    for (auto v : vec) c->add_values(v);
                }
                else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                {
                    auto* c = df.add_stringcolumns();
                    setup_col(c, col.name);
                    c->mutable_values()->Reserve(static_cast<int>(vec.size()));
                    for (const auto& v : vec) c->add_values(v);
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<uint8_t>>>)
                {
                    auto* c = df.add_structcolumns();
                    setup_col(c, col.name);
                    c->set_schemaid("");
                    c->mutable_values()->Reserve(static_cast<int>(vec.size()));
                    for (const auto& blob : vec) c->add_values(blob.data(), blob.size());
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<double>>>)
                {
                    auto* c = df.add_doublearraycolumns();
                    setup_col(c, col.name);
                    std::size_t total = 0; for (const auto& a : vec) total += a.size();
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(total), 0.0);
                    double* dst = vals->mutable_data();
                    for (const auto& arr : vec) { std::memcpy(dst, arr.data(), arr.size() * sizeof(double)); dst += arr.size(); }
                    apply_dims(c, col.name);
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<float>>>)
                {
                    auto* c = df.add_floatarraycolumns();
                    setup_col(c, col.name);
                    std::size_t total = 0; for (const auto& a : vec) total += a.size();
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(total), 0.0f);
                    float* dst = vals->mutable_data();
                    for (const auto& arr : vec) { std::memcpy(dst, arr.data(), arr.size() * sizeof(float)); dst += arr.size(); }
                    apply_dims(c, col.name);
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<int64_t>>>)
                {
                    auto* c = df.add_int64arraycolumns();
                    setup_col(c, col.name);
                    std::size_t total = 0; for (const auto& a : vec) total += a.size();
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(total), 0);
                    int64_t* dst = vals->mutable_data();
                    for (const auto& arr : vec) { std::memcpy(dst, arr.data(), arr.size() * sizeof(int64_t)); dst += arr.size(); }
                    apply_dims(c, col.name);
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<int32_t>>>)
                {
                    auto* c = df.add_int32arraycolumns();
                    setup_col(c, col.name);
                    std::size_t total = 0; for (const auto& a : vec) total += a.size();
                    auto* vals = c->mutable_values();
                    vals->Resize(static_cast<int>(total), 0);
                    int32_t* dst = vals->mutable_data();
                    for (const auto& arr : vec) { std::memcpy(dst, arr.data(), arr.size() * sizeof(int32_t)); dst += arr.size(); }
                    apply_dims(c, col.name);
                }
                else if constexpr (std::is_same_v<T, std::vector<std::vector<bool>>>)
                {
                    auto* c = df.add_boolarraycolumns();
                    setup_col(c, col.name);
                    c->mutable_values()->Reserve(static_cast<int>(vec.size()));
                    for (const auto& arr : vec) for (auto v : arr) c->add_values(v);
                    apply_dims(c, col.name);
                }
            },
            col.values);
    }

    // Enum columns
    for (const auto& ecol : batch.enum_columns)
    {
        auto* c = df.add_enumcolumns();
        setup_col(c, ecol.name);
        c->set_enumid(ecol.enum_id);
        c->mutable_values()->Reserve(static_cast<int>(ecol.values.size()));
        for (auto v : ecol.values) c->add_values(v);
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

    if (!hasAnyColumn(*dataFrame_ptr))
    {
        warnf(logger(), "No valid columns for source {}, skipping request", sourceName);
        return false;
    }

    if (!hasTimestampList(*dataFrame_ptr))
    {
        errorf(logger(), "Dropping frame for source {}: missing DataFrame.datatimestamps.timestamplist", sourceName);
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
    const double depth = static_cast<double>(queueDepth());
    metric_call(metrics_, [&](auto& m)
                {
                    m.setWriterQueueDepth(depth, {{"writer", config_.name}});
                });
}
