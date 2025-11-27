#pragma once

#include <util/pool/IObjectPool.h>
#include <util/pool/IPoolHandle.h>

#include <condition_variable>
#include <functional>
#include <grpcpp/grpcpp.h>
#include <ingestion.grpc.pb.h>
#include <mutex>
#include <vector>

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
 * connection) and at least one stub bound to the channel. Use this pool
 * when you require transport isolation between logical clients (each
 * pooled object maps to a separate TCP/HTTP2 connection).
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
 * This concrete pool implementation maintains a collection of
 * `MLDPGrpcObject` instances. Each pooled object contains its own
 * `grpc::Channel` so that pooled elements are transport-isolated (separate
 * TCP/HTTP2 connections). The pool grows up to `max_size` using the
 * provided factory and blocks callers when all objects are in use.
 *
 * Factory signature:
 * - `using Factory = std::function<ObjectShrdPtr()>;`
 * - The factory must return a `std::shared_ptr<MLDPGrpcObject>` with a
 *   valid `channel` already constructed (and optionally a default `stub`).
 *
 * Lifecycle and semantics:
 * - `acquire()` returns a `PooledHandle<MLDPGrpcObject>`. The handle
 *   ensures the pooled object is returned to the pool when it goes out of
 *   scope (RAII) — callers do not need to call `release()` manually in
 *   typical use.
 * - If the pool has capacity (`current_size < max_size`), `acquire()` may
 *   create a new object via the factory. If capacity is exhausted and all
 *   objects are in use, `acquire()` blocks until an object becomes
 *   available.
 */
class MLDPGrpcPool : public IObjectPool<MLDPGrpcObject>
{
public:
    using ObjectShrdPtr = typename IObjectPool<MLDPGrpcObject>::ObjectShrdPtr;
    using Factory = std::function<ObjectShrdPtr()>; // how to create a new Stub

    /**
     * @brief Construct the pool.
     *
     * @param min_size Minimum number of objects to pre-create and keep
     *                 available in the pool.
     * @param max_size Maximum number of objects the pool may create.
     * @param factory  Callable used to create new `MLDPGrpcObject`
     *                 instances when the pool grows.
     */
    MLDPGrpcPool(std::size_t min_size,
                 std::size_t max_size,
                 Factory     factory);

    /**
     * @brief Acquire a pooled object wrapped in an RAII handle.
     *
     * The returned `PooledHandle` holds the pooled object until it is
     * destroyed; destruction returns the object to the pool automatically.
     * This simplifies usage and prevents accidental leaks of pooled
     * resources.
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

private:
    struct Item
    {
        ObjectShrdPtr obj;
        bool          in_use{false};
    };

    std::size_t min_size_;
    std::size_t max_size_;
    Factory     factory_;

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::vector<Item>       items_;
    std::size_t             current_size_{0};
};

} // namespace mldp_pvxs_driver::util::pool