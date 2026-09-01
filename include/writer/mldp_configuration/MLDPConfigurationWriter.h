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

#include <config/Config.h>
#include <pool/MLDPGrpcAnnotationPool.h>
#include <util/log/Logger.h>
#include <writer/BaseQueuedWriter.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_configuration/MLDPConfigurationWriterConfig.h>

#include <memory>
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
    void doSaveConfiguration(const util::bus::ConfigurationPayload& cfg);
    void doSaveConfigurationActivation(const util::bus::ConfigurationActivationPayload& act);

    MLDPConfigurationWriterConfig                       config_;
    std::shared_ptr<metrics::Metrics>                   metrics_;
    std::shared_ptr<util::pool::MLDPGrpcAnnotationPool> pool_;
};

} // namespace mldp_pvxs_driver::writer
