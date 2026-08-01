//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
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
    ScopedQueryInterruptHandler();
    ~ScopedQueryInterruptHandler();

    ScopedQueryInterruptHandler(const ScopedQueryInterruptHandler&) = delete;
    ScopedQueryInterruptHandler& operator=(const ScopedQueryInterruptHandler&) = delete;

    bool consumeInterrupt() noexcept;

private:
    using Handler = void (*)(int);

    Handler previous_;
};

} // namespace mldp_pvxs_driver::cli
