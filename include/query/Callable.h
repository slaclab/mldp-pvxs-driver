//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/ExpressionRegistry.h>

namespace mldp_pvxs_driver::query {

class Callable final : public IExpressionCallable
{
public:
    explicit Callable(ExpressionCallableDescriptor descriptor);

    const ExpressionCallableDescriptor& descriptor() const noexcept override;

private:
    ExpressionCallableDescriptor descriptor_;
};

} // namespace mldp_pvxs_driver::query
