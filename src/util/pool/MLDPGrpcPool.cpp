#include <util/pool/IObjectPool.h>
#include <util/pool/IPoolHandle.h>
#include <util/pool/MLDPGrpcPool.h>

#include <metrics/Metrics.h>

#include <algorithm>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <stdexcept>

namespace mldp_pvxs_driver::util::pool {

#pragma region - MLDPGrpcObject
// Implement MLDPGrpcObject constructor and helper
MLDPGrpcObject::MLDPGrpcObject() = default;

MLDPGrpcObject::MLDPGrpcObject(std::shared_ptr<grpc::Channel> ch)
    : channel(std::move(ch))
{
    // create a default stub for convenience
    auto iface = std::static_pointer_cast<grpc::ChannelInterface>(channel);
    stub = dp::service::ingestion::DpIngestionService::NewStub(iface);
}

std::unique_ptr<dp::service::ingestion::DpIngestionService::Stub> MLDPGrpcObject::makeStub() const
{
    auto iface = std::static_pointer_cast<grpc::ChannelInterface>(channel);
    return dp::service::ingestion::DpIngestionService::NewStub(iface);
}

#pragma endregion - MLDPGrpcObject

#pragma region - MLDPGrpcPool

MLDPGrpcPool::MLDPGrpcPool(std::size_t                       min_size,
                           std::size_t                       max_size,
                           Factory                           factory,
                           std::shared_ptr<metrics::Metrics> metrics)
    : min_size_(min_size)
    , max_size_(max_size)
    , factory_(std::move(factory))
    , metrics_(std::move(metrics))
{
    if (!factory_)
    {
        throw std::invalid_argument("MLDPGrpcPool: factory is null");
    }
    if (min_size_ == 0 || min_size_ > max_size_)
    {
        throw std::invalid_argument("MLDPGrpcPool: invalid min/max size");
    }

    // Pre-create min_size objects
    items_.reserve(max_size_);
    for (std::size_t i = 0; i < min_size_; ++i)
    {
        items_.push_back({factory_(), false});
    }
    current_size_ = min_size_;
    updateMetrics();
}

MLDPGrpcPool::MLDPGrpcPoolShrdPtr MLDPGrpcPool::create(std::size_t                       min_size,
                                                       std::size_t                       max_size,
                                                       Factory                           factory,
                                                       std::shared_ptr<metrics::Metrics> metrics)
{
    // Use new + shared_ptr constructor because the constructor is private.
    return std::shared_ptr<MLDPGrpcPool>(new MLDPGrpcPool(min_size, max_size, std::move(factory), std::move(metrics)));
}

PooledHandle<MLDPGrpcObject> MLDPGrpcPool::acquire()
{
    std::unique_lock<std::mutex> lock(mutex_);

    for (;;)
    {
        // 1. Try to find an idle object
        for (auto& item : items_)
        {
            if (!item.in_use && item.obj)
            {
                item.in_use = true;
                updateMetricsLocked();
                auto pool_ptr = std::static_pointer_cast<IObjectPool<MLDPGrpcObject>>(shared_from_this());
                return PooledHandle<MLDPGrpcObject>(pool_ptr, item.obj);
            }
        }

        // 2. No idle object; can we create a new one?
        if (current_size_ < max_size_)
        {
            auto obj = factory_();
            items_.push_back({obj, true});
            ++current_size_;
            updateMetricsLocked();
            auto pool_ptr = std::static_pointer_cast<IObjectPool<MLDPGrpcObject>>(shared_from_this());
            return PooledHandle<MLDPGrpcObject>(pool_ptr, obj);
        }

        // 3. Pool is at max and all busy → wait with timeout to remain
        // responsive to spurious wakeups and allow periodic checks. If the
        // timeout expires we simply loop and re-check conditions.
        constexpr auto wait_duration = std::chrono::milliseconds(200);
        cv_.wait_for(lock, wait_duration);
    }
}

void MLDPGrpcPool::release(const ObjectShrdPtr& obj)
{
    if (!obj)
        return;

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

    // Object not from this pool; ignore or assert.
    // assert(false && "MLDPGrpcPool::release: object not from this pool");
}

std::size_t MLDPGrpcPool::available() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return availableCountLocked();
}

std::size_t MLDPGrpcPool::size() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return current_size_;
}

std::size_t MLDPGrpcPool::availableCountLocked() const
{
    std::size_t count = 0;
    for (const auto& item : items_)
    {
        if (!item.in_use && item.obj)
            ++count;
    }
    return count;
}

void MLDPGrpcPool::updateMetricsLocked() const
{
    if (!metrics_)
    {
        return;
    }

    const double total = static_cast<double>(current_size_);
    const double available = static_cast<double>(availableCountLocked());
    metrics_->setPoolConnectionsAvailable(available);
    metrics_->setPoolConnectionsInUse(std::max(0.0, total - available));
}

void MLDPGrpcPool::updateMetrics() const
{
    std::unique_lock<std::mutex> lock(mutex_);
    updateMetricsLocked();
}

#pragma endregion - MLDPGrpcPool

} // namespace mldp_pvxs_driver::util::pool
