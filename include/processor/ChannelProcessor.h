//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file ChannelProcessor.h
 * @brief Writer-compatible processor that turns source updates into algorithm outputs.
 */

#pragma once

#include <BS_thread_pool.hpp>
#include <metrics/Metrics.h>
#include <processor/IAlgorithm.h>
#include <processor/IChannelProcessor.h>
#include <processor/InputBuffer.h>
#include <processor/MLDPChannelProcessorConfig.h>
#include <util/bus/IDataBus.h>
#include <util/log/ILog.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace mldp_pvxs_driver::processor {

/**
 * @class ChannelProcessor
 * @brief Base processor runtime used by algorithm-backed virtual channels.
 */
class ChannelProcessor final : public IChannelProcessor
{
public:
    ChannelProcessor(MLDPChannelProcessorConfig                config,
                     IAlgorithmUPtr                          algorithm,
                     std::shared_ptr<util::bus::IDataBus>    bus,
                     std::shared_ptr<metrics::Metrics>       metrics,
                     std::shared_ptr<BS::light_thread_pool>  thread_pool);
    ~ChannelProcessor() override;

    std::string name() const override;
    void        start() override;
    void        stop() noexcept override;
    bool        push(util::bus::IDataBus::EventBatch batch) noexcept override;
    bool        acceptsPayload(const util::bus::BatchPayload& payload) const noexcept override;
    bool        supports_multi_root_source() const noexcept override;

    const std::string&              outputReaderName() const noexcept override;
    std::vector<std::string>        outputSourceNames() const noexcept override;
    const std::vector<std::string>& inputSourceNames() const noexcept override;
    void                            setEffectiveSources(std::vector<std::string> sources) override;

private:
    void fireCompute(const AlignedSnapshot& snapshot) noexcept;
    void processTask(util::bus::IDataBus::EventBatch batch) noexcept;

    MLDPChannelProcessorConfig              config_;
    IAlgorithmUPtr                          algorithm_;
    std::shared_ptr<util::bus::IDataBus>    bus_;
    std::shared_ptr<metrics::Metrics>       metrics_;
    std::shared_ptr<util::log::ILogger>     logger_;
    std::shared_ptr<BS::light_thread_pool>  thread_pool_;
    InputBuffer                             buffer_;
    std::atomic<bool>                       running_{false};
    std::atomic<int>                        pending_tasks_{0};
    std::mutex                              drain_mutex_;
    std::condition_variable                 drain_cv_;
};

} // namespace mldp_pvxs_driver::processor
