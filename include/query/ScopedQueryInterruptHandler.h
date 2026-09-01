//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file ScopedQueryInterruptHandler.h
 * @brief Declares scoped SIGINT handling for query cancellation. */
#pragma once

#include <csignal>

namespace mldp_pvxs_driver::cli {

/** @brief Installs a SIGINT handler and exposes pending query interruption requests. */
class ScopedQueryInterruptHandler
{
public:
    /** @brief Installs a SIGINT handler that sets a pending interrupt flag. */
    ScopedQueryInterruptHandler();

    /** @brief Restores the previous SIGINT handler. */
    ~ScopedQueryInterruptHandler();

    ScopedQueryInterruptHandler(const ScopedQueryInterruptHandler&) = delete;
    ScopedQueryInterruptHandler& operator=(const ScopedQueryInterruptHandler&) = delete;

    /** @brief Returns true and clears the interrupt flag if SIGINT was received.
     * @return True if an interrupt was pending. */
    bool consumeInterrupt() noexcept;

private:
    using Handler = void (*)(int);

    Handler previous_;  ///< SIGINT handler that was in place before this object was constructed.
};

} // namespace mldp_pvxs_driver::cli
