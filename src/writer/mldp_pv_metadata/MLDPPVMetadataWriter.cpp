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

#include <metrics/Metrics.h>
#include <annotation.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <pool/MLDPGrpcAnnotationPoolConfig.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <chrono>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::metrics;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MLDPPVMetadataWriter::~MLDPPVMetadataWriter()
{
    stop();
}

MLDPPVMetadataWriter::MLDPPVMetadataWriter(const config::Config&             root,
                                           std::shared_ptr<metrics::Metrics> metrics)
    : MLDPPVMetadataWriter(MLDPPVMetadataWriterConfig::parse(root), std::move(metrics))
{
}

MLDPPVMetadataWriter::MLDPPVMetadataWriter(MLDPPVMetadataWriterConfig        config,
                                           std::shared_ptr<metrics::Metrics> metrics)
    : BaseQueuedWriter<WorkItem>(
          QueueConfig{1000, std::max(1, config.threadPool), 0},
          "mldp-pv-metadata:" + config.name,
          newLogger("pv_metadata_writer:" + config.name))
    , config_(std::move(config))
    , metrics_(std::move(metrics))
{
}

// ---------------------------------------------------------------------------
// BaseQueuedWriter hooks
// ---------------------------------------------------------------------------

void MLDPPVMetadataWriter::doStart()
{
    pool_ = MLDPGrpcAnnotationPool::create(config_.poolConfig, metrics_);
    infof(logger(), "MLDPPVMetadataWriter '{}' pool ready", config_.name);
}

void MLDPPVMetadataWriter::doStop() noexcept
{
    pool_.reset();
    infof(logger(), "MLDPPVMetadataWriter '{}' stopped", config_.name);
}

std::vector<MLDPPVMetadataWriter::WorkItem>
MLDPPVMetadataWriter::toItems(IDataBus::EventBatch& batch)
{
    const auto* meta = std::get_if<SourceMetadataPayload>(&batch.payload);
    if (!meta)
    {
        tracef(logger(), "MLDPPVMetadataWriter '{}' discarding non-metadata payload from '{}'",
               config_.name, batch.reader_name);
        return {};
    }

    std::vector<WorkItem> items;
    items.reserve(meta->sources.size());
    for (const auto& [sourceName, entry] : meta->sources)
    {
        tracef(logger(), "MLDPPVMetadataWriter '{}' enqueuing '{}' ({} attrs)",
               config_.name, sourceName, entry.attributes.size());
        items.emplace_back(sourceName, entry);
    }
    return items;
}

void MLDPPVMetadataWriter::processItem(std::size_t /*workerIndex*/, WorkItem item)
{
    tracef(logger(), "MLDPPVMetadataWriter '{}' worker saving '{}' ({} attrs)",
           config_.name, item.first, item.second.attributes.size());
    saveSourceMetadata(item.first, item.second);
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
            errorf(logger(),
                   "MLDPPVMetadataWriter savePvMetadata '{}': gRPC error {}: {}",
                   sourceName,
                   static_cast<int>(status.error_code()),
                   status.error_message());
            metric_call(metrics_, [&](auto& m) {
                m.incrementWriterFailures(1.0, {{"writer", config_.name}});
            });
            return;
        }
        metric_call(metrics_, [&](auto& m) {
            m.incrementWriterPushes(1.0, {{"writer", config_.name}});
        });
    }
    catch (const std::exception& ex)
    {
        errorf(logger(),
               "MLDPPVMetadataWriter saveSourceMetadata '{}' exception: {}",
               sourceName, ex.what());
        metric_call(metrics_, [&](auto& m) {
            m.incrementWriterFailures(1.0, {{"writer", config_.name}});
        });
    }
}
