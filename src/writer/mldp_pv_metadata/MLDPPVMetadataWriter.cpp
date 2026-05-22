//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/mldp_pv_metadata/MLDPPVMetadataWriter.h>

#include <annotation.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <util/log/Logger.h>

#include <chrono>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::pool;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MLDPPVMetadataWriter::MLDPPVMetadataWriter(const config::Config&             root,
                                           std::shared_ptr<metrics::Metrics> metrics)
    : config_(MLDPPVMetadataWriterConfig::parse(root))
    , metrics_(std::move(metrics))
    , logger_(newLogger("pv_metadata_writer:" + config_.name))
{
}

MLDPPVMetadataWriter::~MLDPPVMetadataWriter()
{
    if (running_.load())
    {
        stop();
    }
}

// ---------------------------------------------------------------------------
// IWriter lifecycle
// ---------------------------------------------------------------------------

void MLDPPVMetadataWriter::start()
{
    if (running_.load())
    {
        warnf(*logger_, "MLDPPVMetadataWriter '{}' already started", config_.name);
        return;
    }

    pool_ = MLDPGrpcAnnotationPool::create(config_.poolConfig, metrics_);
    stop_.store(false);
    running_.store(true);

    const int count = std::max(1, config_.threadPool);
    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        workers_.emplace_back([this]
                              {
                                  workerLoop();
                              });
    }

    infof(*logger_, "MLDPPVMetadataWriter '{}' started ({} workers)",
          config_.name, count);
}

void MLDPPVMetadataWriter::stop() noexcept
{
    stop_.store(true);
    queue_cv_.notify_all();
    for (auto& t : workers_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    workers_.clear();
    running_.store(false);
    infof(*logger_, "MLDPPVMetadataWriter '{}' stopped", config_.name);
}

// ---------------------------------------------------------------------------
// push
// ---------------------------------------------------------------------------

bool MLDPPVMetadataWriter::push(IDataBus::EventBatch batch) noexcept
{
    const auto* meta = std::get_if<SourceMetadataPayload>(&batch.payload);
    if (!meta)
    {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& [sourceName, entry] : *meta)
        {
            work_queue_.push({sourceName, entry});
        }
    }
    queue_cv_.notify_all();
    return true;
}

// ---------------------------------------------------------------------------
// workerLoop
// ---------------------------------------------------------------------------

void MLDPPVMetadataWriter::workerLoop()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this]
                       {
                           return stop_.load() || !work_queue_.empty();
                       });

        if (work_queue_.empty())
        {
            // stop_ was set and queue is empty — exit
            return;
        }

        WorkItem item = std::move(work_queue_.front());
        work_queue_.pop();
        lock.unlock();

        saveSourceMetadata(item.source_name, item.entry);
    }
}

// ---------------------------------------------------------------------------
// saveSourceMetadata
// ---------------------------------------------------------------------------

void MLDPPVMetadataWriter::saveSourceMetadata(const std::string&         sourceName,
                                              const SourceMetadataEntry& entry)
{
    try
    {
        auto handle = pool_->acquire();

        dp::service::annotation::SavePvMetadataRequest req;
        req.set_pvname(sourceName);

        if (entry.aliases)
        {
            for (const auto& a : *entry.aliases)
            {
                req.add_aliases(a);
            }
        }

        if (entry.tags)
        {
            for (const auto& t : *entry.tags)
            {
                req.add_tags(t);
            }
        }

        for (const auto& [k, v] : entry.attributes)
        {
            auto* attr = req.add_attributes();
            attr->set_name(k);
            attr->set_value(v);
        }

        if (entry.description)
        {
            req.set_description(*entry.description);
        }

        if (entry.modified_by)
        {
            req.set_modifiedby(*entry.modified_by);
        }

        dp::service::annotation::SavePvMetadataResponse resp;
        grpc::ClientContext                             ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->savePvMetadata(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(*logger_,
                   "MLDPPVMetadataWriter savePvMetadata '{}': gRPC error {}: {}",
                   sourceName,
                   static_cast<int>(status.error_code()),
                   status.error_message());
        }
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_,
               "MLDPPVMetadataWriter saveSourceMetadata '{}' exception: {}",
               sourceName, ex.what());
    }
}
