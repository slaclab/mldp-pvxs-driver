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

#include <config/Config.h>

#include <pvxs/client.h>

#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::reader::impl::epics {

/** Builds an isolated PVXS client configuration from one reader entry. */
class PvxsClientConfig
{
public:
    /** Exception raised when a reader's PVXS environment map is invalid. */
    struct Error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    /** Validate the optional environment map without creating a PVXS context. */
    static void validate(const config::Config& readerEntry, const std::string& readerType);

    /** Apply reader-local overrides to the inherited process PVXS settings. */
    static pvxs::client::Config buildConfig(const config::Config& readerEntry, const std::string& readerType);

    /** Build a PVXS context using inherited settings plus reader-local overrides. */
    static pvxs::client::Context buildContext(const config::Config& readerEntry, const std::string& readerType);
};

} // namespace mldp_pvxs_driver::reader::impl::epics
