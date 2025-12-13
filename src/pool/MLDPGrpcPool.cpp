#include <pool/IObjectPool.h>
#include <pool/IPoolHandle.h>
#include <pool/MLDPGrpcPool.h>

#include <metrics/Metrics.h>

#include <algorithm>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <stdexcept>

#include <util/log/Logger.h>

using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::util::log;

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

MLDPGrpcPool::MLDPGrpcPool(const MLDPGrpcPoolConfig&         config,
                           std::shared_ptr<metrics::Metrics> metrics)
    : config_(config)
    , metrics_(std::move(metrics))
{
    if (!config.valid())
    {
        throw std::invalid_argument("MLDPGrpcPool: configuration is invalid");
    }
    if (config_.minConnections() == 0 || config_.minConnections() > config_.maxConnections())
    {
        throw std::invalid_argument("MLDPGrpcPool: invalid min/max size");
    }

    // Pre-create min_size objects
    items_.reserve(config_.maxConnections());
    for (std::size_t i = 0; i < config_.minConnections(); ++i)
    {
        items_.push_back({createChannel(), false});
    }
    current_size_ = config_.minConnections();
    updateMetrics();
}

MLDPGrpcPool::MLDPGrpcPoolShrdPtr MLDPGrpcPool::create(const MLDPGrpcPoolConfig&         config,
                                                       std::shared_ptr<metrics::Metrics> metrics)
{
    // Use new + shared_ptr constructor because the constructor is private.
    return std::shared_ptr<MLDPGrpcPool>(new MLDPGrpcPool(config, std::move(metrics)));
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
        if (current_size_ < config_.maxConnections())
        {
            auto obj = createChannel();
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

std::shared_ptr<MLDPGrpcObject> MLDPGrpcPool::createChannel()
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

    auto channel = grpc::CreateChannel(config_.url(), creds);
    auto object = std::make_shared<MLDPGrpcObject>(channel);
    if (!registerProvider(object->stub.get()))
    {
        throw std::runtime_error("MLDPGrpcPool: failed to register provider on new connection");
    }
    return object;
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

bool MLDPGrpcPool::registerProvider(dp::service::ingestion::DpIngestionService::Stub* stub)
{
    if (!stub)
    {
        errorf("MLDPGrpcPool: missing stub while registering provider {}", config_.providerName());
        return false;
    }

    grpc::ClientContext                              context;
    dp::service::ingestion::RegisterProviderRequest  request;
    dp::service::ingestion::RegisterProviderResponse response;
    request.set_providername(config_.providerName());
    request.set_description(config_.providerDescription());

    const auto status = stub->registerProvider(&context, request, &response);
    if (!status.ok())
    {
        errorf("registerProvider RPC failed for {}: {}", config_.providerName(), status.error_message());
        return false;
    }

    if (response.has_registrationresult())
    {
        const auto& new_id = response.registrationresult().providerid();
        if (provider_id_.empty())
        {
            provider_id_ = new_id;
            infof("MLDP registered provider {} with id {}", config_.providerName(), provider_id_);
            return true;
        }
        if (provider_id_ != new_id)
        {
            errorf("registerProvider returned mismatched provider ID {} (expected {})", new_id, provider_id_);
            return false;
        }
        return true;
    }

    if (response.has_exceptionalresult())
    {
        errorf("registerProvider rejected {}: {}", config_.providerName(), response.exceptionalresult().message());
        return false;
    }

    errorf("registerProvider returned empty response for {}", config_.providerName());
    return false;
}

const std::string& MLDPGrpcPool::providerId() const
{
    return provider_id_;
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
