//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/NullQueryCommandListener.h>

using namespace mldp_pvxs_driver::cli;

void NullQueryCommandListener::querySubmitted(std::string_view) {}
void NullQueryCommandListener::progressChanged(const query::QueryProgressSnapshot&) {}
void NullQueryCommandListener::resultBatchAvailable(const QueryResultBatchDescriptor&) {}
void NullQueryCommandListener::queryCompleted(const query::QueryStats&) {}
void NullQueryCommandListener::queryCancelled() {}
void NullQueryCommandListener::queryFailed(std::string_view) {}
void NullQueryCommandListener::queryIdle() {}
