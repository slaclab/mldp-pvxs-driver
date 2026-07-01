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
    : BaseQueuedWriter<QueueItem>(
          QueueConfig{static_cast<int>(config.queueCapacity), config.threadPoolSize},
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
    infof(logger(), "MLDPWriter provider registered: {}", providerId_);
}

void MLDPWriter::doStop() noexcept
{
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

void MLDPWriter::processItem(std::size_t /*workerIndex*/, QueueItem item)
{
    // StreamState is per-worker; keep it in a thread_local so each worker
    // maintains its own gRPC stream across processItem calls.
    thread_local StreamState state;

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
        return;
    }

    if (std::chrono::steady_clock::now() - state.streamStart >= config_.streamMaxAge)
    {
        closeStream(state, "stream age exceeded");
        if (!ensureStream(state))
        {
            record_send_time({{"source", "unknown"}});
            return;
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
        return;
    }

    if ((state.streamPayloadBytes + payloadBytes) > config_.streamMaxBytes &&
        state.streamPayloadBytes > 0)
    {
        closeStream(state, "max bytes exceeded");
        if (!ensureStream(state))
        {
            record_send_time({{"source", "unknown"}});
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
            if (delta_sec > 0.0)
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
            prev = event_sec;
    }
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
    int64_t requestedRequests = static_cast<int64_t>(state.requestCounter);
    if (status.ok())
    {
        if (state.response.has_ingestdatastreamresult())
        {
            const auto& result = state.response.ingestdatastreamresult();
            if (result.numrequests() < 0)
            {
                errorf(logger(), "Ingestion stream finished with invalid numrequests ({}): {}", reason, result.numrequests());
            }
            else if (result.numrequests() < requestedRequests)
            {
                errorf(logger(), "Ingestion stream finished with incomplete requests ({}): server accepted {} of {} sent",
                       reason, result.numrequests(), requestedRequests);
            }
            else if (result.numrequests() > requestedRequests)
            {
                errorf(logger(), "Ingestion stream finished with mismatch ({}): server reports {} but we sent {}",
                       reason, result.numrequests(), requestedRequests);
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
                            m.incrementWriterFailures(1.0, {{"writer", config_.name}, {"source", "unknown"}});
                        });
        }
    }
    else
    {
        errorf(logger(), "Ingestion stream finished with error ({}): {}", reason, status.error_message());
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
            errorf(logger(), "Failed to open ingestion stream");
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
        errorf(logger(), "Failed to acquire ingestion stream: {}", ex.what());
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
        warnf(logger(), "No valid columns for source {}, skipping request", sourceName);
        return false;
    }

    if (!hasTimestampListW(*dataFrame_ptr))
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
