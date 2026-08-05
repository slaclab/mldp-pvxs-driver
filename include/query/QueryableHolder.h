//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryableHolder.h
 * @brief Holds a queryable instance with its table registrations. */
#pragma once
#include <query/IQueryable.h>

namespace mldp_pvxs_driver::query {

/** @brief Associates one queryable instance with the tables it serves. */
class QueryableHolder
{
public:
    QueryableHolder() = default;

    explicit QueryableHolder(IQueryableUPtr impl)
        : impl_(std::move(impl)) {}

    /** @brief Returns true if this holder contains a non-null queryable.
     *  @return True when impl_ is non-null. */
    bool valid() const
    {
        return impl_ != nullptr;
    }

    /** @brief Returns a pointer to the stored impl cast to T, or nullptr if the impl is not a T.
     *  @tparam T Target queryable type.
     *  @return Pointer of type T*, or nullptr. */
    template <typename T>
    T* as() const
    {
        return dynamic_cast<T*>(impl_.get());
    }

private:
    IQueryableUPtr impl_; ///< Owned queryable implementation.
};

} // namespace mldp_pvxs_driver::query
