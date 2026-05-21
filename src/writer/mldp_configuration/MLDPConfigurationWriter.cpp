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

#include <annotation.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::pool;

namespace {

/// Minimal overloaded-lambda helper for std::visit.
template <class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MLDPConfigurationWriter::MLDPConfigurationWriter(const config::Config&             root,
                                                  std::shared_ptr<metrics::Metrics> metrics)
    : config_(MLDPConfigurationWriterConfig::parse(root))
    , metrics_(std::move(metrics))
    , logger_(newLogger("configuration_writer:" + config_.name))
{
}

MLDPConfigurationWriter::~MLDPConfigurationWriter()
{
    if (running_.load())
    {
        stop();
    }
}

// ---------------------------------------------------------------------------
// IWriter lifecycle
// ---------------------------------------------------------------------------

void MLDPConfigurationWriter::start()
{
    if (running_.load())
    {
        warnf(*logger_, "MLDPConfigurationWriter '{}' already started", config_.name);
        return;
    }

    pool_ = MLDPGrpcAnnotationPool::create(config_.poolConfig, metrics_);
    stop_.store(false);
    running_.store(true);

    const int count = std::max(1, config_.threadPool);
    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        workers_.emplace_back([this] { workerLoop(); });
    }
    infof(*logger_, "MLDPConfigurationWriter '{}' started ({} workers)",
          config_.name, count);
}

void MLDPConfigurationWriter::stop() noexcept
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
    infof(*logger_, "MLDPConfigurationWriter '{}' stopped", config_.name);
}

bool MLDPConfigurationWriter::push(IDataBus::EventBatch batch) noexcept
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (auto* cfg = std::get_if<ConfigurationPayload>(&batch.payload))
        {
            work_queue_.push(WorkItem{*cfg});
        }
        else if (auto* act = std::get_if<ConfigurationActivationPayload>(&batch.payload))
        {
            work_queue_.push(WorkItem{*act});
        }
        else
        {
            // Payload type not handled by this writer — silently ignore.
            return true;
        }
    }
    queue_cv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void MLDPConfigurationWriter::workerLoop()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return stop_.load() || !work_queue_.empty(); });

        if (work_queue_.empty())
        {
            // stop_ was set and queue is drained — exit.
            return;
        }

        auto item = std::move(work_queue_.front());
        work_queue_.pop();
        lock.unlock();

        std::visit(overloaded{
                       [this](const ConfigurationPayload& cfg) { doSaveConfiguration(cfg); },
                       [this](const ConfigurationActivationPayload& act) {
                           doSaveConfigurationActivation(act);
                       }},
                   item.data);
    }
}

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
        grpc::ClientContext                                 ctx;
        ctx.set_deadline(std::chrono::system_clock::now()
                         + std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->saveConfiguration(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(*logger_,
                   "MLDPConfigurationWriter saveConfiguration '{}': gRPC error {}: {}",
                   cfg.configuration_name,
                   static_cast<int>(status.error_code()),
                   status.error_message());
        }
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_,
               "MLDPConfigurationWriter saveConfiguration '{}' exception: {}",
               cfg.configuration_name, ex.what());
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
        grpc::ClientContext                                           ctx;
        ctx.set_deadline(std::chrono::system_clock::now()
                         + std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->saveConfigurationActivation(&ctx, req, &resp);
        if (!status.ok())
        {
            errorf(*logger_,
                   "MLDPConfigurationWriter saveConfigurationActivation '{}': gRPC error {}: {}",
                   act.configuration_name,
                   static_cast<int>(status.error_code()),
                   status.error_message());
        }
    }
    catch (const std::exception& ex)
    {
        errorf(*logger_,
               "MLDPConfigurationWriter saveConfigurationActivation '{}' exception: {}",
               act.configuration_name, ex.what());
    }
}
