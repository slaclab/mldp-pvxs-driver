//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <pool/IObjectPool.h>
#include <pool/IPoolHandle.h>
#include <pool/MLDPGrpcAnnotationPool.h>

#include <metrics/Metrics.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <stdexcept>

using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::util::log;

namespace {
std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeMLDPGRPCAnnotationLogger()
{
    std::string loggerName = "mldp_grpc_annotation_pool";
    return mldp_pvxs_driver::util::log::newLogger(loggerName);
}

const prometheus::Labels kAnnotationPoolMetricLabels{{"pool", "annotation"}};
} // namespace

MLDPGrpcAnnotationObject::MLDPGrpcAnnotationObject(std::shared_ptr<grpc::Channel> ch)
    : channel(ch)
    , stub(dp::service::annotation::DpAnnotationService::NewStub(ch))
{
}

MLDPGrpcAnnotationPool::MLDPGrpcAnnotationPool(const MLDPGrpcPoolConfig&         config,
                                               std::shared_ptr<metrics::Metrics> metrics)
    : logger_(makeMLDPGRPCAnnotationLogger())
    , config_(config)
    , metrics_(std::move(metrics))
{
    if (!config.valid())
    {
        throw std::invalid_argument("MLDPGrpcAnnotationPool: configuration is invalid");
    }
    if (config_.annotationUrl().empty())
    {
        throw std::invalid_argument("MLDPGrpcAnnotationPool: annotation-url must not be empty");
    }
    if (config_.minConnections() == 0 || config_.minConnections() > config_.maxConnections())
    {
        throw std::invalid_argument("MLDPGrpcAnnotationPool: invalid min/max size");
    }

    items_.reserve(config_.maxConnections());
    for (std::size_t i = 0; i < config_.minConnections(); ++i)
    {
        items_.push_back({createChannel(), false});
    }

    current_size_ = config_.minConnections();
    updateMetrics();
}

MLDPGrpcAnnotationPool::MLDPGrpcAnnotationPoolShrdPtr
MLDPGrpcAnnotationPool::create(const MLDPGrpcPoolConfig&         config,
                               std::shared_ptr<metrics::Metrics> metrics)
{
    return std::shared_ptr<MLDPGrpcAnnotationPool>(new MLDPGrpcAnnotationPool(config, std::move(metrics)));
}

PooledHandle<MLDPGrpcAnnotationObject> MLDPGrpcAnnotationPool::acquire()
{
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;)
    {
        for (auto& item : items_)
        {
            if (!item.in_use && item.obj)
            {
                item.in_use = true;
                updateMetricsLocked();
                auto pool_ptr = std::static_pointer_cast<IObjectPool<MLDPGrpcAnnotationObject>>(shared_from_this());
                return PooledHandle<MLDPGrpcAnnotationObject>(pool_ptr, item.obj);
            }
        }

        if (current_size_ < config_.maxConnections())
        {
            auto obj = createChannel();
            items_.push_back({obj, true});
            ++current_size_;
            updateMetricsLocked();
            auto pool_ptr = std::static_pointer_cast<IObjectPool<MLDPGrpcAnnotationObject>>(shared_from_this());
            return PooledHandle<MLDPGrpcAnnotationObject>(pool_ptr, obj);
        }

        constexpr auto wait_duration = std::chrono::milliseconds(200);
        cv_.wait_for(lock, wait_duration);
    }
}

std::shared_ptr<MLDPGrpcAnnotationObject> MLDPGrpcAnnotationPool::createChannel()
{
    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (config_.credentials().type == MLDPGrpcPoolConfig::Credentials::Type::Ssl)
    {
        creds = grpc::SslCredentials(config_.credentials().ssl_options);
    }
    else
    {
        creds = grpc::InsecureChannelCredentials();
    }

    auto channel = grpc::CreateChannel(config_.annotationUrl(), creds);
    return std::make_shared<MLDPGrpcAnnotationObject>(channel);
}

void MLDPGrpcAnnotationPool::release(const ObjectShrdPtr& obj)
{
    if (!obj)
    {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    for (auto& item : items_)
    {
        if (item.obj.get() == obj.get())
        {
            item.in_use = false;
            updateMetricsLocked();
            cv_.notify_one();
            return;
        }
    }
}

std::size_t MLDPGrpcAnnotationPool::available() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return availableCountLocked();
}

std::size_t MLDPGrpcAnnotationPool::size() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return current_size_;
}

std::size_t MLDPGrpcAnnotationPool::availableCountLocked() const
{
    std::size_t count = 0;
    for (const auto& item : items_)
    {
        if (!item.in_use && item.obj)
        {
            ++count;
        }
    }
    return count;
}

void MLDPGrpcAnnotationPool::updateMetricsLocked() const
{
    if (!metrics_)
    {
        return;
    }

    const double total = static_cast<double>(current_size_);
    const double available = static_cast<double>(availableCountLocked());
    metrics_->setPoolConnectionsAvailable(available, kAnnotationPoolMetricLabels);
    metrics_->setPoolConnectionsInUse(std::max(0.0, total - available), kAnnotationPoolMetricLabels);
}

void MLDPGrpcAnnotationPool::updateMetrics() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    updateMetricsLocked();
}
