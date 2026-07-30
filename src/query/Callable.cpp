//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/Callable.h>

#include <utility>

using namespace mldp_pvxs_driver::query;

Callable::Callable(ExpressionCallableDescriptor descriptor)
    : descriptor_(std::move(descriptor))
{
}

const ExpressionCallableDescriptor& Callable::descriptor() const noexcept
{
    return descriptor_;
}
