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

#include <memory>

namespace mldp_pvxs_driver::query {

// Factory marker only. Use QueryableFactory::create<T>() for concrete access.
class IQueryable
{
public:
    virtual ~IQueryable() = default;
};

using IQueryableUPtr = std::unique_ptr<IQueryable>;

} // namespace mldp_pvxs_driver::query
