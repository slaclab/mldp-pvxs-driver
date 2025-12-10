#pragma once

#include <pool/IObjectPool.h>
#include <pool/IPoolHandle.h>
#include <pool/MLDPGrpcPoolConfig.h>

#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <ingestion.grpc.pb.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::metrics {
class Metrics;
} // namespace mldp_pvxs_driver::metrics

namespace mldp_pvxs_driver::util::pool {

/**
 * @brief Pooled connection object.
 *
 * MLDPGrpcObject encapsulates a single transport connection (a
 * `grpc::Channel`) together with a convenience stub for issuing RPCs on
 * that connection. Instances of this type are owned by the pool as
 * `std::shared_ptr<MLDPGrpcObject>`; each pooled element therefore
 * represents a separate transport/connection to the server.
 *
 * Typical usage:
 * - The pool creates one `MLDPGrpcObject` per pooled connection using a
 *   factory callable.
 * - Callers acquire a `PooledHandle<MLDPGrpcObject>` from the pool to
 *   reserve the connection; the handle returns the object to the pool on
 *   destruction.
 *
 * Members:
 * - `channel`: the gRPC channel (transport). Each pooled object should
 *   construct its own channel to ensure transport isolation.
 * - `stub`: a convenience `unique_ptr` to a stub bound to `channel`.
 *
 * Threading/ownership:
 * - The `channel` and any stubs created from it follow gRPC's
 *   thread-safety guarantees and may be used concurrently by multiple
 *   threads. The `MLDPGrpcObject` itself is owned by `std::shared_ptr`
 *   and managed by the pool; callers should not attempt to delete it
 *   directly.
 */
struct MLDPGrpcObject
{
    std::shared_ptr<grpc::Channel>                                    channel;
    std::unique_ptr<dp::service::ingestion::DpIngestionService::Stub> stub;

    /** Default-construct an empty pooled object. */
    MLDPGrpcObject();

    /**
     * @brief Construct a pooled object that owns `channel` and creates a
     * default stub bound to it.
     *
     * @param channel Shared pointer to the gRPC channel (transport) for
     *                this pooled object.
     */
    MLDPGrpcObject(std::shared_ptr<grpc::Channel> channel);

    /**
     * @brief Create a new stub that uses the same underlying channel.
     *
     * Returns a fresh `unique_ptr` to a stub instance that will send RPCs
     * over this object's channel.
     */
    /**
     * @brief Create a new stub that uses the same underlying channel.
     *
     * Returns a fresh `unique_ptr` to a stub that issues RPCs over this
     * object's channel. Useful when multiple stub handles are required
     * concurrently while sharing the same transport connection.
     *
     * @return unique_ptr to a new `DpIngestionService::Stub` bound to
     *         this object's channel.
     */
    std::unique_ptr<dp::service::ingestion::DpIngestionService::Stub> makeStub() const;
};

/**
 * @brief Concrete pool that manages `MLDPGrpcObject` instances.
 *
 * Each pooled object contains its own `grpc::Channel` (separate transport
 * connection) and a convenience stub bound to that channel. Use this pool
 * when you require transport isolation between logical clients — each
 * pooled element represents a separate TCP/HTTP2 connection to the
 * server.
 *
 * Ownership and construction notes:
 * - This pool must be created and owned by a `std::shared_ptr` because
 *   `acquire()` uses `shared_from_this()` to return handles that keep a
 *   reference to the originating pool. To enforce that, construction is
 *   performed via the static `create()` factory which returns a
 *   `std::shared_ptr<MLDPGrpcPool>`.
 * - Callers should prefer `MLDPGrpcPool::create(...)` and keep the
 *   resulting `shared_ptr` for the pool's lifetime. Returned handles
 *   (`PooledHandle<MLDPGrpcObject>`) will also hold a `shared_ptr` to
 *   the pool, guaranteeing the pool remains alive while any handle is
 *   outstanding.
 *
 * Factory
 * - The pool takes a `Factory` callable that returns `ObjectShrdPtr` (a
 *   `std::shared_ptr<MLDPGrpcObject>`). The factory should construct a
 *   channel and a stub for that channel and return a shared pointer to the
 *   object.
 *
 * Acquire / Release
 * - `acquire()` returns a `PooledHandle<MLDPGrpcObject>` which reserves
 *   the pooled object until the handle is destroyed; the handle's
 *   destructor calls `release()` automatically. This removes the need for
 *   manual `release()` calls in common code paths.
 * - If the pool has no idle objects and `current_size < max_size`, it
 *   creates new objects using the factory. If the pool is at `max_size`
 *   and all objects are in use, `acquire()` blocks until one becomes
 *   available.
 *
 * Threading
 * - The pool is internally synchronized: multiple threads may call
 *   `acquire()` concurrently. The returned `PooledHandle` should not be
 *   accessed concurrently, but stubs/channels inside the pooled object may
 *   be used by multiple threads following gRPC's thread-safety rules.
 */
/**
 * @brief Pool that manages `MLDPGrpcObject` instances (one connection each).
 *
 * The pool bootstraps itself from a validated @ref MLDPGrpcPoolConfig. The
 * configuration drives the endpoint URL as well as the minimum/maximum
 * connection counts. Each pooled object owns its own `grpc::Channel`, so the
 * pool provides transport-isolated connections that are recycled via
 * `PooledHandle`.
 *
 * Lifecycle and semantics:
 * - Use @ref create to instantiate the pool. It automatically pre-populates
 *   `min_conn` connections and grows up to `max_conn` using the config.
 * - `acquire()` returns a `PooledHandle<MLDPGrpcObject>`. The handle returns
 *   the connection to the pool when destroyed (RAII).
 * - If the pool has capacity (`current_size < max_conn`), `acquire()` creates
 *   new connections as needed; otherwise it blocks until an idle connection is
 *   available. Each new connection automatically registers the configured
 *   provider with MLDP and caches the provider ID for later reuse.
 */
class MLDPGrpcPool : public IObjectPool<MLDPGrpcObject>, public std::enable_shared_from_this<MLDPGrpcPool>
{
public:
    using MLDPGrpcPoolShrdPtr = std::shared_ptr<MLDPGrpcPool>;
    using ObjectShrdPtr = typename IObjectPool<MLDPGrpcObject>::ObjectShrdPtr;

    /**
     * @brief Create a new managed `MLDPGrpcPool` instance.
    *
     * Use this factory to construct the pool. It returns a `std::shared_ptr`
     * so that `acquire()` can safely create handles that retain a reference
     * to the pool via `shared_from_this()`.
     *
     * @param config    Configuration that determines the endpoint URL,
     *                  minimum/maximum connections, and other pool behavior.
     * @param metrics   Optional metrics collector that receives pool statistics.
     * @return std::shared_ptr<MLDPGrpcPool> Managed pool instance.
    */
    static MLDPGrpcPoolShrdPtr create(const MLDPGrpcPoolConfig&         config,
                                      std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Acquire a pooled object wrapped in an RAII handle.
     *
     * The returned `PooledHandle` reserves the pooled object until the
     * handle is destroyed; destruction returns the object to the pool
     * automatically by calling `release()` on the originating pool.
     * The handle stores a `std::shared_ptr<IObjectPool<MLDPGrpcObject>>`
     * to keep the pool alive while the handle exists — callers therefore
     * do not need to manage pool lifetime while objects are checked out.
     *
     * Blocking semantics:
     * - If an idle object exists it is returned immediately.
     * - If no idle object exists and the pool can grow (`current_size < max_size`),
     *   a new object is created via the provided factory and returned.
     * - If the pool is at `max_size` and all objects are in use, `acquire()`
     *   blocks until an object becomes available.
     *
     * @return PooledHandle<MLDPGrpcObject> RAII wrapper for the pooled object.
     */
    PooledHandle<MLDPGrpcObject> acquire() override;

    /**
     * @brief Release a previously acquired pooled object back to the pool.
     *
     * This is called by the `PooledHandle` destructor; callers normally
     * don't need to call it directly unless using manual `ObjectShrdPtr`
     * semantics.
     *
     * @param obj Shared pointer to the pooled object to release.
     */
    void release(const ObjectShrdPtr& obj) override;

    /**
     * @brief Return the number of currently available (idle) objects in the pool.
     *
     * See `IObjectPool::available()`.
     */
    std::size_t available() const override;

    /**
     * @brief Return the number of connections currently tracked by the pool.
     */
    std::size_t size() const;

    /**
     * @brief Retrieve the provider ID obtained during automatic registration.
     *
     * @return Assigned provider identifier or an empty string if registration failed.
     */
    const std::string& providerId() const;

private:
    const MLDPGrpcPoolConfig        config_;
    std::size_t                     availableCountLocked() const;
    void                            updateMetricsLocked() const;
    void                            updateMetrics() const;
    std::shared_ptr<MLDPGrpcObject> createChannel();
    // Make constructor private to force use of `create()` which returns a
    // `std::shared_ptr` (required for `enable_shared_from_this`).
    MLDPGrpcPool(const MLDPGrpcPoolConfig&         config,
                 std::shared_ptr<metrics::Metrics> metrics);

    struct Item
    {
        ObjectShrdPtr obj;
        bool          in_use{false};
    };

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::vector<Item>       items_;
    std::size_t             current_size_{0};
    std::string             provider_id_; ///< Last provider ID returned by MLDP.
    // Token used to detect pool lifetime. Pooled handles keep a weak_ptr to
    // this token so they can safely avoid calling back into the pool after
    // the pool has been destroyed.
    std::shared_ptr<void>             lifetime_token_;
    std::shared_ptr<metrics::Metrics> metrics_;

    bool registerProvider(dp::service::ingestion::DpIngestionService::Stub* stub);
};

} // namespace mldp_pvxs_driver::util::pool
