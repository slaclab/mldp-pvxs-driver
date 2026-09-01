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
#include <pool/MLDPGrpcAnnotationPoolConfig.h>
#include <pool/MLDPGrpcPoolConfig.h>
#include <util/log/Logger.h>

#include <annotation.grpc.pb.h>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <vector>

namespace mldp_pvxs_driver::metrics {
class Metrics;
} // namespace mldp_pvxs_driver::metrics

namespace mldp_pvxs_driver::util::pool {

/**
 * @brief Pooled connection object for the annotation service.
 *
 * Holds a single gRPC channel and a DpAnnotationService stub bound to it.
 */
struct MLDPGrpcAnnotationObject
{
    std::shared_ptr<grpc::Channel>                                      channel;
    std::unique_ptr<dp::service::annotation::DpAnnotationService::Stub> stub;

    MLDPGrpcAnnotationObject() = default;
    explicit MLDPGrpcAnnotationObject(std::shared_ptr<grpc::Channel> ch);
};

/**
 * @brief Connection pool for DpAnnotationService.
 *
 * Mirrors MLDPGrpcQueryPool but manages MLDPGrpcAnnotationObject instances
 * and connects to the annotation endpoint from MLDPGrpcPoolConfig::annotationUrl().
 *
 * Must be created via the static create() factory (requires shared_ptr ownership
 * for enable_shared_from_this).
 */
class MLDPGrpcAnnotationPool
    : public IObjectPool<MLDPGrpcAnnotationObject>,
      public std::enable_shared_from_this<MLDPGrpcAnnotationPool>
{
public:
    using MLDPGrpcAnnotationPoolShrdPtr = std::shared_ptr<MLDPGrpcAnnotationPool>;
    using ObjectShrdPtr = typename IObjectPool<MLDPGrpcAnnotationObject>::ObjectShrdPtr;

    static MLDPGrpcAnnotationPoolShrdPtr create(const MLDPGrpcAnnotationPoolConfig& config,
                                                std::shared_ptr<metrics::Metrics>   metrics = nullptr);

    /** Convenience overload for callers that hold a full MLDPGrpcPoolConfig. */
    static MLDPGrpcAnnotationPoolShrdPtr create(const MLDPGrpcPoolConfig&         config,
                                                std::shared_ptr<metrics::Metrics> metrics = nullptr);

    PooledHandle<MLDPGrpcAnnotationObject> acquire() override;
    void                                   release(const ObjectShrdPtr& obj) override;
    std::size_t                            available() const override;
    std::size_t                            size() const;

private:
    struct Item
    {
        ObjectShrdPtr obj;
        bool          in_use{false};
    };

    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;
    const MLDPGrpcAnnotationPoolConfig                    config_;
    mutable std::mutex                                    mutex_;
    std::condition_variable                               cv_;
    std::vector<Item>                                     items_;
    std::size_t                                           current_size_{0};
    std::shared_ptr<metrics::Metrics>                     metrics_;

    MLDPGrpcAnnotationPool() = delete;
    MLDPGrpcAnnotationPool(const MLDPGrpcAnnotationPool&) = delete;
    MLDPGrpcAnnotationPool& operator=(const MLDPGrpcAnnotationPool&) = delete;

    MLDPGrpcAnnotationPool(const MLDPGrpcAnnotationPoolConfig& config,
                           std::shared_ptr<metrics::Metrics>   metrics);
    std::size_t                               availableCountLocked() const;
    void                                      updateMetricsLocked() const;
    void                                      updateMetrics() const;
    std::shared_ptr<MLDPGrpcAnnotationObject> createChannel();
};

} // namespace mldp_pvxs_driver::util::pool
