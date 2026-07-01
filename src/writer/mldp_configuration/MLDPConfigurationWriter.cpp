//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/mldp_configuration/MLDPConfigurationWriter.h>

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

MLDPConfigurationWriter::~MLDPConfigurationWriter()
{
    stop();
}

MLDPConfigurationWriter::MLDPConfigurationWriter(const config::Config&             root,
                                                 std::shared_ptr<metrics::Metrics> metrics)
    : MLDPConfigurationWriter(MLDPConfigurationWriterConfig::parse(root), std::move(metrics))
{
}

MLDPConfigurationWriter::MLDPConfigurationWriter(MLDPConfigurationWriterConfig     config,
                                                 std::shared_ptr<metrics::Metrics> metrics)
    : BaseQueuedWriter<ConfigItem>(
          QueueConfig{1000, std::max(1, config.threadPool), 0},
          "mldp-configuration:" + config.name,
          newLogger("configuration_writer:" + config.name))
    , config_(std::move(config))
    , metrics_(std::move(metrics))
{
}

// ---------------------------------------------------------------------------
// BaseQueuedWriter hooks
// ---------------------------------------------------------------------------

void MLDPConfigurationWriter::doStart()
{
    pool_ = MLDPGrpcAnnotationPool::create(config_.poolConfig, metrics_);
    infof(logger(), "MLDPConfigurationWriter '{}' pool ready", config_.name);
}

void MLDPConfigurationWriter::doStop() noexcept
{
    pool_.reset();
    infof(logger(), "MLDPConfigurationWriter '{}' stopped", config_.name);
}

std::vector<MLDPConfigurationWriter::ConfigItem>
MLDPConfigurationWriter::toItems(IDataBus::EventBatch& batch)
{
    if (auto* cfg = std::get_if<ConfigurationPayload>(&batch.payload))
    {
        return {ConfigItem{*cfg}};
    }
    if (auto* act = std::get_if<ConfigurationActivationPayload>(&batch.payload))
    {
        return {ConfigItem{*act}};
    }
    return {};
}

void MLDPConfigurationWriter::processItem(std::size_t /*workerIndex*/, ConfigItem item)
{
    std::visit(
        [this](auto&& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ConfigurationPayload>)
            {
                doSaveConfiguration(payload);
            }
            else if constexpr (std::is_same_v<T, ConfigurationActivationPayload>)
            {
                doSaveConfigurationActivation(payload);
            }
        },
        item);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void MLDPConfigurationWriter::doSaveConfiguration(const ConfigurationPayload& cfg)
{
    try
    {
        auto handle = pool_->acquire();

        dp::service::annotation::SaveConfigurationRequest req;
        req.set_configurationname(cfg.configuration_name);
        req.set_category(cfg.category);
        if (cfg.description)
        {
            req.set_description(*cfg.description);
        }
        if (cfg.parent_configuration_name)
        {
            req.set_parentconfigurationname(*cfg.parent_configuration_name);
        }
        if (cfg.tags)
        {
            for (const auto& t : *cfg.tags)
            {
                req.add_tags(t);
            }
        }
        for (const auto& [k, v] : cfg.attributes)
        {
            auto* attr = req.add_attributes();
            attr->set_name(k);
            attr->set_value(v);
        }
        if (cfg.modified_by)
        {
            req.set_modifiedby(*cfg.modified_by);
        }

        dp::service::annotation::SaveConfigurationResponse resp;
        grpc::ClientContext                                ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->saveConfiguration(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(logger(),
                   "MLDPConfigurationWriter saveConfiguration '{}': gRPC error {}: {}",
                   cfg.configuration_name,
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
               "MLDPConfigurationWriter saveConfiguration '{}' exception: {}",
               cfg.configuration_name, ex.what());
        metric_call(metrics_, [&](auto& m) {
            m.incrementWriterFailures(1.0, {{"writer", config_.name}});
        });
    }
}

void MLDPConfigurationWriter::doSaveConfigurationActivation(
    const ConfigurationActivationPayload& act)
{
    try
    {
        auto handle = pool_->acquire();

        dp::service::annotation::SaveConfigurationActivationRequest req;
        if (act.client_activation_id)
        {
            req.set_clientactivationid(*act.client_activation_id);
        }
        req.set_configurationname(act.configuration_name);

        auto* st = req.mutable_starttime();
        st->set_epochseconds(act.start_time.epoch_seconds);
        st->set_nanoseconds(act.start_time.nanoseconds);

        if (act.end_time)
        {
            auto* et = req.mutable_endtime();
            et->set_epochseconds(act.end_time->epoch_seconds);
            et->set_nanoseconds(act.end_time->nanoseconds);
        }
        if (act.description)
        {
            req.set_description(*act.description);
        }
        if (act.tags)
        {
            for (const auto& t : *act.tags)
            {
                req.add_tags(t);
            }
        }
        for (const auto& [k, v] : act.attributes)
        {
            auto* attr = req.add_attributes();
            attr->set_name(k);
            attr->set_value(v);
        }
        if (act.modified_by)
        {
            req.set_modifiedby(*act.modified_by);
        }

        dp::service::annotation::SaveConfigurationActivationResponse resp;
        grpc::ClientContext                                          ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->saveConfigurationActivation(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(logger(),
                   "MLDPConfigurationWriter saveConfigurationActivation '{}': gRPC error {}: {}",
                   act.configuration_name,
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
               "MLDPConfigurationWriter saveConfigurationActivation '{}' exception: {}",
               act.configuration_name, ex.what());
        metric_call(metrics_, [&](auto& m) {
            m.incrementWriterFailures(1.0, {{"writer", config_.name}});
        });
    }
}
