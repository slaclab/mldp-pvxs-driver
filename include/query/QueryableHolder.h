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

    bool valid() const
    {
        return impl_ != nullptr;
    }

    // Returns T* if stored impl is-a T, else nullptr. Never throws.
    template <typename T>
    T* as() const
    {
        return dynamic_cast<T*>(impl_.get());
    }

private:
    IQueryableUPtr impl_;
};

} // namespace mldp_pvxs_driver::query
