//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file Callable.h
 * @brief Provides a concrete immutable SQL expression callable. */
#pragma once

#include <query/ExpressionRegistry.h>

namespace mldp_pvxs_driver::query {

/** @brief Stores the descriptor for one registered expression callable. */
class Callable final : public IExpressionCallable
{
public:
    /** @brief Constructs a callable from an immutable descriptor.
     * @param[in] descriptor Callable descriptor. */
    explicit Callable(ExpressionCallableDescriptor descriptor);

    /** @brief Returns the callable's immutable descriptor.
     * @return Const reference to the stored descriptor. */
    const ExpressionCallableDescriptor& descriptor() const noexcept override;

private:
    ExpressionCallableDescriptor descriptor_;  ///< Stored callable descriptor.
};

} // namespace mldp_pvxs_driver::query
