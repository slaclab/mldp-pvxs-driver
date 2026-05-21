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
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/mldp_configuration/MLDPConfigurationWriterConfig.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <variant>
#include <vector>

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
class MLDPConfigurationWriter final : public IWriter
{
    REGISTER_WRITER("mldp-configuration", MLDPConfigurationWriter)
public:
    /**
     * @brief Factory constructor — parses config from the root YAML node.
     *
     * Called by the @ref WriterFactory registry.
     */
    explicit MLDPConfigurationWriter(const config::Config&             root,
                                     std::shared_ptr<metrics::Metrics> metrics = nullptr);
    ~MLDPConfigurationWriter() override;

    std::string name() const override
    {
        return config_.name;
    }

    void start() override;
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void stop() noexcept override;

    bool acceptsPayload(const util::bus::BatchPayload& p) const noexcept override
    {
        return std::holds_alternative<util::bus::ConfigurationPayload>(p) || std::holds_alternative<util::bus::ConfigurationActivationPayload>(p);
    }

private:
    /// Discriminated payload carried by each work item.
    using WorkData = std::variant<util::bus::ConfigurationPayload,
                                  util::bus::ConfigurationActivationPayload>;

    /// One unit of queued work.
    struct WorkItem
    {
        WorkData data;
    };

    /// Worker thread loop: dequeues and dispatches items until stopped.
    void workerLoop();

    /// Issue a saveConfiguration RPC for the given payload.
    void doSaveConfiguration(const util::bus::ConfigurationPayload& cfg);

    /// Issue a saveConfigurationActivation RPC for the given payload.
    void doSaveConfigurationActivation(const util::bus::ConfigurationActivationPayload& act);

    MLDPConfigurationWriterConfig                       config_;
    std::shared_ptr<metrics::Metrics>                   metrics_;
    std::shared_ptr<util::log::ILogger>                 logger_;
    std::shared_ptr<util::pool::MLDPGrpcAnnotationPool> pool_;

    std::queue<WorkItem>     work_queue_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool>        stop_{false};
    std::atomic<bool>        running_{false};
};

} // namespace mldp_pvxs_driver::writer
