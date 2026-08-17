//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/ScopedQueryInterruptHandler.h>

using namespace mldp_pvxs_driver::cli;

namespace {

volatile std::sig_atomic_t g_query_interrupt_requested = 0;

extern "C" void requestQueryInterrupt(int)
{
    g_query_interrupt_requested = 1;
}

} // namespace

ScopedQueryInterruptHandler::ScopedQueryInterruptHandler()
    : previous_(std::signal(SIGINT, requestQueryInterrupt))
{
    g_query_interrupt_requested = 0;
}

ScopedQueryInterruptHandler::~ScopedQueryInterruptHandler()
{
    std::signal(SIGINT, previous_);
}

bool ScopedQueryInterruptHandler::consumeInterrupt() noexcept
{
    if (g_query_interrupt_requested == 0) return false;
    g_query_interrupt_requested = 0;
    return true;
}
