// PooledHandle.h
#pragma once

#include <memory>

namespace mldp_pvxs_driver::util::pool {
template <typename T>
class IObjectPool;

/**
 * @brief RAII wrapper for objects acquired from an `IObjectPool`.
 *
 * `PooledHandle<T>` holds a pooled object obtained from an `IObjectPool<T>`
 * and returns it to the pool when the handle is destroyed or `reset()` is
 * called. The handle is move-only to ensure a single owner returns the
 * object to the pool.
 *
 * Usage:
 * - Construct from a pool pointer and a `PoolObjectPtr` obtained via
 *   `IObjectPool<T>::acquire()`.
 * - Move the handle to transfer ownership.
 * - Call `reset()` to explicitly return the object to the pool before the
 *   handle is destroyed.
 *
 * @tparam T Type of the pooled object.
 */
template <typename T>
class PooledHandle
{
public:
    using Pool = IObjectPool<T>;
    using PoolObjectPtr = typename IObjectPool<T>::PoolObjectPtr;

    /** Default-construct an empty handle (no object owned). */
    PooledHandle() = default;

    /**
     * @brief Construct a handle that owns `obj` from `pool`.
     *
     * @param pool Pointer to the originating pool (may be nullptr for
     *             non-pooled ownership semantics).
     * @param obj  Smart pointer to the pooled object.
     */
    PooledHandle(std::shared_ptr<Pool> pool, PoolObjectPtr obj)
        : pool_(std::move(pool)), obj_(std::move(obj)) {}

    /** Move-only: transfer ownership from another handle. */
    PooledHandle(PooledHandle&& other) noexcept
        : pool_(std::move(other.pool_)), obj_(std::move(other.obj_)) {}

    /** Move-only assignment: release current object then take other's. */
    PooledHandle& operator=(PooledHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            pool_ = std::move(other.pool_);
            obj_ = std::move(other.obj_);
        }
        return *this;
    }

    PooledHandle(const PooledHandle&) = delete;
    PooledHandle& operator=(const PooledHandle&) = delete;

    /**
     * @brief Destructor returns the owned object to the pool if present.
     */
    ~PooledHandle()
    {
        reset();
    }

    /**
     * @brief Access the underlying object pointer.
     *
     * @return T* pointer to the managed object (may be null if empty).
     */
    T* operator->()
    {
        return obj_.get();
    }

    /**
     * @brief Dereference the underlying object.
     *
     * @return T& reference to the managed object.
     */
    T& operator*()
    {
        return *obj_;
    }

    const T* operator->() const
    {
        return obj_.get();
    }

    const T& operator*() const
    {
        return *obj_;
    }

    /**
     * @brief Test whether this handle currently owns an object.
     */
    explicit operator bool() const noexcept
    {
        return static_cast<bool>(obj_);
    }

    /**
     * @brief Return the object to the pool (if any) and clear ownership.
     *
     * If the handle holds an object and has a non-null pool pointer, the
     * object is returned to the pool via `pool_->release(...)`. The local
     * smart pointer is then reset.
     */
    void reset()
    {
        if (pool_ && obj_)
        {
            pool_->release(obj_);
            obj_.reset();
            pool_.reset();
        }
    }

private:
    std::shared_ptr<Pool> pool_;
    PoolObjectPtr obj_;
    
};
} // namespace mldp_pvxs_driver::util::pool