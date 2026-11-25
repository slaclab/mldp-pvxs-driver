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
#include <ctime>
#include <thread>
#include <unordered_map>

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

namespace {

/// gRPC status codes worth retrying (transport / server-load problems).
bool isTransientStatus(grpc::StatusCode code)
{
    switch (code)
    {
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
    case grpc::StatusCode::ABORTED:
    case grpc::StatusCode::INTERNAL:
    case grpc::StatusCode::UNKNOWN:
        return true;
    default:
        return false;
    }
}

std::string statusMessage(const grpc::Status& status)
{
    return "gRPC error " + std::to_string(static_cast<int>(status.error_code())) + ": " +
           status.error_message();
}

std::string toIsoUtc(const BusTimestamp& ts)
{
    const auto t = static_cast<std::time_t>(ts.epoch_seconds);
    std::tm    u{};
    gmtime_r(&t, &u);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &u);
    return buf;
}

} // namespace

MLDPConfigurationWriter::LookupResult
MLDPConfigurationWriter::fetchExistingConfiguration(const std::string&        configurationName,
                                                    MLDPGrpcAnnotationObject& conn)
{
    try
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(config_.deadlineSeconds));
        dp::service::annotation::GetConfigurationRequest  req;
        req.set_configurationname(configurationName);
        dp::service::annotation::GetConfigurationResponse resp;
        const auto status = conn.stub->getConfiguration(&ctx, req, &resp);
        if (!status.ok())
        {
            if (isTransientStatus(status.error_code()))
                return {LookupResult::Kind::Error, std::nullopt,
                        "existing-configuration lookup failed, " + statusMessage(status)};
            // Non-transient (e.g. UNIMPLEMENTED, NOT_FOUND): retrying won't help;
            // fall back to saving as a new configuration, but make it visible.
            warnf(logger(),
                  "MLDPConfigurationWriter fetchExistingConfiguration '{}': {} — saving as new",
                  configurationName, statusMessage(status));
            return {LookupResult::Kind::NotFound, std::nullopt, {}};
        }
        if (!resp.has_getconfigurationresult())
        {
            // Server answered (typically "not found"): treat as a new configuration.
            tracef(logger(), "MLDPConfigurationWriter fetchExistingConfiguration '{}': not found",
                   configurationName);
            return {LookupResult::Kind::NotFound, std::nullopt, {}};
        }
        return {LookupResult::Kind::Found, resp.getconfigurationresult().configuration(), {}};
    }
    catch (const std::exception& ex)
    {
        return {LookupResult::Kind::Error, std::nullopt,
                std::string("existing-configuration lookup failed: ") + ex.what()};
    }
}

MLDPConfigurationWriter::SaveResult
MLDPConfigurationWriter::runWithRetry(const char* op, const std::string& name,
                                      const std::function<SaveResult()>& attempt,
                                      int& attempts)
{
    const int  maxAttempts = 1 + std::max(0, config_.maxRetries);
    auto       backoff     = std::chrono::milliseconds(std::max(0, config_.retryBackoffMs));
    SaveResult result;
    for (attempts = 1;; ++attempts)
    {
        result = attempt();
        if (result.outcome != SaveOutcome::Transient || attempts >= maxAttempts)
            return result;

        warnf(logger(),
              "MLDPConfigurationWriter {} '{}' attempt {}/{} failed: {} — retrying in {} ms",
              op, name, attempts, maxAttempts, result.message, backoff.count());
        {
            std::lock_guard<std::mutex> lk(statsMutex_);
            ++statRetried_;
        }
        std::this_thread::sleep_for(backoff);
        backoff *= 2;
    }
}

void MLDPConfigurationWriter::recordOutcome(bool isConfiguration, bool ok)
{
    metric_call(metrics_, [&](auto& m) {
        if (ok)
            m.incrementWriterPushes(1.0, {{"writer", config_.name}});
        else
            m.incrementWriterFailures(1.0, {{"writer", config_.name}});
    });

    std::lock_guard<std::mutex> lk(statsMutex_);
    if (!ok)
        ++statFailed_;
    else if (isConfiguration)
        ++statConfigsSaved_;
    else
        ++statActivationsSaved_;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastStatsLogTime_ >= std::chrono::seconds(10))
    {
        infof(logger(),
              "MLDPConfigurationWriter '{}': last 10s saved configurations={} activations={} "
              "failed={} retried={}",
              config_.name, statConfigsSaved_, statActivationsSaved_, statFailed_, statRetried_);
        statConfigsSaved_ = statActivationsSaved_ = statFailed_ = statRetried_ = 0;
        lastStatsLogTime_ = now;
    }
}

MLDPConfigurationWriter::SaveResult
MLDPConfigurationWriter::attemptSaveConfiguration(const ConfigurationPayload& cfg)
{
    try
    {
        auto handle = pool_->acquire();

        const auto lookup = fetchExistingConfiguration(cfg.configuration_name, *handle);
        if (lookup.kind == LookupResult::Kind::Error)
        {
            // Never save blind: a "new" save would overwrite the existing
            // description/tags/attributes we failed to read.
            return {SaveOutcome::Transient, lookup.message};
        }
        const auto& existing = lookup.configuration;

        dp::service::annotation::SaveConfigurationRequest req;
        req.set_configurationname(cfg.configuration_name);
        req.set_category(cfg.category);

        if (existing)
        {
            // description: incoming if present, else existing
            req.set_description(
                cfg.description ? *cfg.description : existing->description());

            // parent_configuration_name: incoming if present, else existing
            req.set_parentconfigurationname(
                cfg.parent_configuration_name
                    ? *cfg.parent_configuration_name
                    : existing->parentconfigurationname());

            // tags: incoming wins if present, else preserve existing
            if (cfg.tags)
            {
                for (const auto& t : *cfg.tags)
                    req.add_tags(t);
            }
            else
            {
                for (const auto& t : existing->tags())
                    req.add_tags(t);
            }

            // attributes: merge — existing as base, incoming overwrites/adds
            std::unordered_map<std::string, std::string> merged;
            for (const auto& attr : existing->attributes())
                merged[attr.name()] = attr.value();
            for (const auto& [k, v] : cfg.attributes)
                merged[k] = v;
            for (const auto& [k, v] : merged)
            {
                auto* attr = req.add_attributes();
                attr->set_name(k);
                attr->set_value(v);
            }

            // modified_by: incoming if present, else existing
            req.set_modifiedby(
                cfg.modified_by ? *cfg.modified_by : existing->modifiedby());
        }
        else
        {
            if (cfg.description)
                req.set_description(*cfg.description);
            if (cfg.parent_configuration_name)
                req.set_parentconfigurationname(*cfg.parent_configuration_name);
            if (cfg.tags)
            {
                for (const auto& t : *cfg.tags)
                    req.add_tags(t);
            }
            for (const auto& [k, v] : cfg.attributes)
            {
                auto* attr = req.add_attributes();
                attr->set_name(k);
                attr->set_value(v);
            }
            if (cfg.modified_by)
                req.set_modifiedby(*cfg.modified_by);
        }

        dp::service::annotation::SaveConfigurationResponse resp;
        grpc::ClientContext                                ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->saveConfiguration(&ctx, req, &resp);
        if (!status.ok())
        {
            return {isTransientStatus(status.error_code()) ? SaveOutcome::Transient
                                                           : SaveOutcome::Rejected,
                    statusMessage(status)};
        }
        if (!resp.has_saveconfigurationresult())
        {
            return {SaveOutcome::Rejected,
                    resp.has_exceptionalresult() ? resp.exceptionalresult().message()
                                                 : "annotation service returned no save result"};
        }
        return {SaveOutcome::Ok, {}};
    }
    catch (const std::exception& ex)
    {
        return {SaveOutcome::Transient, std::string("exception: ") + ex.what()};
    }
}

void MLDPConfigurationWriter::doSaveConfiguration(const ConfigurationPayload& cfg)
{
    int        attempts = 0;
    const auto result   = runWithRetry(
        "saveConfiguration", cfg.configuration_name,
        [&] { return attemptSaveConfiguration(cfg); }, attempts);

    if (result.outcome != SaveOutcome::Ok)
    {
        errorf(logger(),
               "MLDPConfigurationWriter saveConfiguration '{}' failed after {} attempt(s){}: {}",
               cfg.configuration_name, attempts,
               result.outcome == SaveOutcome::Rejected ? " (rejected by server)" : "",
               result.message);
    }
    recordOutcome(true, result.outcome == SaveOutcome::Ok);
}

MLDPConfigurationWriter::SaveResult
MLDPConfigurationWriter::attemptSaveConfigurationActivation(
    const dp::service::annotation::SaveConfigurationActivationRequest& req)
{
    try
    {
        auto handle = pool_->acquire();

        dp::service::annotation::SaveConfigurationActivationResponse resp;
        grpc::ClientContext                                          ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(config_.deadlineSeconds));

        const auto status = handle->stub->saveConfigurationActivation(&ctx, req, &resp);
        if (!status.ok())
        {
            return {isTransientStatus(status.error_code()) ? SaveOutcome::Transient
                                                           : SaveOutcome::Rejected,
                    statusMessage(status)};
        }
        if (!resp.has_saveconfigurationactivationresult())
        {
            return {SaveOutcome::Rejected,
                    resp.has_exceptionalresult() ? resp.exceptionalresult().message()
                                                 : "annotation service returned no save result"};
        }
        return {SaveOutcome::Ok, {}};
    }
    catch (const std::exception& ex)
    {
        return {SaveOutcome::Transient, std::string("exception: ") + ex.what()};
    }
}

void MLDPConfigurationWriter::doSaveConfigurationActivation(
    const ConfigurationActivationPayload& act)
{
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

    int        attempts = 0;
    const auto result   = runWithRetry(
        "saveConfigurationActivation", act.configuration_name,
        [&] { return attemptSaveConfigurationActivation(req); }, attempts);

    if (result.outcome == SaveOutcome::Rejected)
    {
        // Typical cause: MLDP refuses an activation overlapping one already stored
        // for the same configuration/category under a different client id
        // (e.g. a re-ingest whose event boundaries differ from the earlier run).
        errorf(logger(),
               "MLDPConfigurationWriter saveConfigurationActivation '{}' rejected by server "
               "(id='{}', start={}, end={}): {}",
               act.configuration_name,
               act.client_activation_id.value_or(""),
               toIsoUtc(act.start_time),
               act.end_time ? toIsoUtc(*act.end_time) : std::string("open"),
               result.message);
    }
    else if (result.outcome == SaveOutcome::Transient)
    {
        errorf(logger(),
               "MLDPConfigurationWriter saveConfigurationActivation '{}' failed after {} "
               "attempt(s) (id='{}', start={}): {}",
               act.configuration_name, attempts,
               act.client_activation_id.value_or(""),
               toIsoUtc(act.start_time), result.message);
    }
    recordOutcome(false, result.outcome == SaveOutcome::Ok);
}
