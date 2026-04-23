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

#include <pool/IObjectPool.h>
#include <pool/IPoolHandle.h>
#include <pool/MLDPGrpcPool.h>
#include <pool/MLDPGrpcPoolConfig.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace mldp_pvxs_driver::metrics {
class Metrics;
} // namespace mldp_pvxs_driver::metrics

namespace mldp_pvxs_driver::util::pool {

class MLDPGrpcQueryPool : public IObjectPool<MLDPGrpcObject>, public std::enable_shared_from_this<MLDPGrpcQueryPool>
{
public:
    using MLDPGrpcQueryPoolShrdPtr = std::shared_ptr<MLDPGrpcQueryPool>;
    using ObjectShrdPtr = typename IObjectPool<MLDPGrpcObject>::ObjectShrdPtr;

    static MLDPGrpcQueryPoolShrdPtr create(const MLDPGrpcPoolConfig&         config,
                                           std::shared_ptr<metrics::Metrics> metrics = nullptr);

    PooledHandle<MLDPGrpcObject> acquire() override;
    void                         release(const ObjectShrdPtr& obj) override;
    std::size_t                  available() const override;
    std::size_t                  size() const;

private:
    struct Item
    {
        ObjectShrdPtr obj;
        bool          in_use{false};
    };

    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;
    const MLDPGrpcPoolConfig                              config_;
    mutable std::mutex                                    mutex_;
    std::condition_variable                               cv_;
    std::vector<Item>                                     items_;
    std::size_t                                           current_size_{0};
    std::shared_ptr<metrics::Metrics>                     metrics_;

    MLDPGrpcQueryPool() = delete;
    MLDPGrpcQueryPool(const MLDPGrpcQueryPool&) = delete;
    MLDPGrpcQueryPool& operator=(const MLDPGrpcQueryPool&) = delete;

    MLDPGrpcQueryPool(const MLDPGrpcPoolConfig&         config,
                      std::shared_ptr<metrics::Metrics> metrics);
    std::size_t                     availableCountLocked() const;
    void                            updateMetricsLocked() const;
    void                            updateMetrics() const;
    std::shared_ptr<MLDPGrpcObject> createChannel();
};

} // namespace mldp_pvxs_driver::util::pool
