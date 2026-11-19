//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <common.pb.h>
#include <config/Config.h>
#include <pool/MLDPGrpcAnnotationPool.h>
#include <util/log/Logger.h>
#include <writer/BaseQueuedWriter.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_configuration/MLDPConfigurationWriterConfig.h>

#include <annotation.pb.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace mldp_pvxs_driver::metrics {
class Metrics;
} // namespace mldp_pvxs_driver::metrics

namespace mldp_pvxs_driver::writer {

/**
 * @brief Writer that persists configuration objects and activation windows via gRPC.
 *
 * Accepts @ref util::bus::ConfigurationPayload and
 * @ref util::bus::ConfigurationActivationPayload batches, dispatching each to
 * the appropriate annotation-service RPC (saveConfiguration /
 * saveConfigurationActivation) through an @ref util::pool::MLDPGrpcAnnotationPool.
 *
 * Lifecycle is identical to other writers: construct → start() → push() → stop().
 */
class MLDPConfigurationWriter final : public BaseQueuedWriter<
    std::variant<util::bus::ConfigurationPayload, util::bus::ConfigurationActivationPayload>>
{
    REGISTER_WRITER("mldp-configuration", MLDPConfigurationWriter)
public:
    using ConfigItem = std::variant<util::bus::ConfigurationPayload,
                                    util::bus::ConfigurationActivationPayload>;

    /**
     * @brief Factory constructor — parses config from the root YAML node.
     *
     * Called by the @ref WriterFactory registry.
     */
    explicit MLDPConfigurationWriter(const config::Config&             root,
                                     std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit MLDPConfigurationWriter(MLDPConfigurationWriterConfig     config,
                                     std::shared_ptr<metrics::Metrics> metrics = nullptr);

    ~MLDPConfigurationWriter() override;

    std::string name() const override
    {
        return config_.name;
    }

    bool acceptsPayload(const util::bus::BatchPayload& p) const noexcept override
    {
        return std::holds_alternative<util::bus::ConfigurationPayload>(p) ||
               std::holds_alternative<util::bus::ConfigurationActivationPayload>(p);
    }

protected:
    std::vector<ConfigItem> toItems(util::bus::IDataBus::EventBatch& batch) override;
    void processItem(std::size_t workerIndex, ConfigItem item) override;
    void doStart() override;
    void doStop() noexcept override;

    std::string itemRoutingKey(const ConfigItem& item) const override
    {
        return std::visit(
            [](const auto& p) -> std::string { return p.configuration_name; },
            item);
    }

private:
    /// Outcome class of a single save attempt.
    enum class SaveOutcome
    {
        Ok,        ///< Saved.
        Transient, ///< Transport/server-load failure; worth retrying.
        Rejected   ///< Server refused the request; retrying will not help.
    };

    struct SaveResult
    {
        SaveOutcome outcome{SaveOutcome::Ok};
        std::string message;
    };

    /// Result of looking up an existing configuration before a save.
    struct LookupResult
    {
        enum class Kind
        {
            Found,
            NotFound,
            Error
        };
        Kind                                              kind{Kind::NotFound};
        std::optional<dp::service::common::Configuration> configuration;
        std::string                                       message;
    };

    void doSaveConfiguration(const util::bus::ConfigurationPayload& cfg);
    void doSaveConfigurationActivation(const util::bus::ConfigurationActivationPayload& act);

    SaveResult attemptSaveConfiguration(const util::bus::ConfigurationPayload& cfg);
    SaveResult attemptSaveConfigurationActivation(
        const dp::service::annotation::SaveConfigurationActivationRequest& req);

    /**
     * @brief Run @p attempt, retrying Transient outcomes with exponential backoff.
     *
     * @param attempts  Set to the number of attempts made.
     */
    SaveResult runWithRetry(const char* op, const std::string& name,
                            const std::function<SaveResult()>& attempt, int& attempts);

    /// Update metrics and the 10 s summary counters; emits the summary log when due.
    void recordOutcome(bool isConfiguration, bool ok);

    LookupResult fetchExistingConfiguration(const std::string&                    configurationName,
                                            util::pool::MLDPGrpcAnnotationObject& conn);

    MLDPConfigurationWriterConfig                       config_;
    std::shared_ptr<metrics::Metrics>                   metrics_;
    std::shared_ptr<util::pool::MLDPGrpcAnnotationPool> pool_;

    std::mutex                            statsMutex_;
    std::chrono::steady_clock::time_point lastStatsLogTime_{std::chrono::steady_clock::now()};
    std::uint64_t                         statConfigsSaved_{0};
    std::uint64_t                         statActivationsSaved_{0};
    std::uint64_t                         statFailed_{0};
    std::uint64_t                         statRetried_{0};
};

} // namespace mldp_pvxs_driver::writer
