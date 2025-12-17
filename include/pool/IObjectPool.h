//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// IObjectPool.h
#pragma once

#include <memory>

namespace mldp_pvxs_driver::util::pool {
template <typename T>
class PooledHandle;

/**
 * @brief Minimal interface for an object pool.
 *
 * Template parameter T is the type of object managed by the pool. The pool
 * hands out shared pointers (`PoolObjectPtr`) to callers; callers should
 * return objects to the pool by calling `release`. Implementations may block
 * in `acquire()` when no objects are available or may create new objects on
 * demand depending on the pool strategy.
 *
 * @tparam T Type of pooled objects.
 */
template <typename T>
class IObjectPool
{
public:
    /**
     * Shared pointer type used to represent pooled objects.
     * Implementations may use a different smart-pointer type, but this
     * interface exposes `std::shared_ptr<T>` for simplicity and safety.
     */
    using ObjectShrdPtr = std::shared_ptr<T>;
    /** Backwards-compatible name for the shared pointer type used across code. */
    using PoolObjectPtr = ObjectShrdPtr;

    virtual ~IObjectPool() = default;

    /**
     * @brief Acquire an object from the pool.
     *
     * Depending on the pool implementation, this may block, throw, or create
     * a new object when the pool is exhausted. The returned `PoolObjectPtr`
     * represents ownership of the object while the caller holds the pointer.
     *
     * @return PoolObjectPtr a smart pointer to the acquired object.
     */
    /**
     * @brief Acquire an object from the pool wrapped in an RAII handle.
     *
     * The returned `PooledHandle<T>` will automatically return the object to
     * the pool when it is destroyed. This eliminates the need for callers
     * to call `release()` manually in the common case.
     */
    virtual PooledHandle<T> acquire() = 0;

    /**
     * @brief Release an object back into the pool.
     *
     * Typically invoked by an RAII wrapper (e.g. `PooledHandle`) when the
     * wrapper is destroyed or reset. The pool may reuse the object, destroy
     * it, or perform other bookkeeping as required by the implementation.
     *
     * @param obj The pooled object to return to the pool.
     */
    virtual void release(const ObjectShrdPtr& obj) = 0;

    /**
     * @brief Return the number of currently available (idle) objects in the pool.
     *
     * This counts objects that are present in the pool and not currently
     * marked in-use. Implementations should return 0 if no idle objects
     * exist.
     */
    virtual std::size_t available() const = 0;
};
} // namespace mldp_pvxs_driver::util::pool