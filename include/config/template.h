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
#include <string_view>

namespace mldp_pvxs_driver::config {

enum class TemplateKind
{
    MldpOnly,      // minimal: one mldp writer + one epics-pvxs reader
    MldpAndHdf5,   // full: mldp + hdf5 writers + pvxs + base readers
    EpicsArchiver, // archiver: historical + periodic_tail modes
};

/// Returns the embedded YAML template text for the given kind.
std::string_view getConfigTemplate(TemplateKind kind);

} // namespace mldp_pvxs_driver::config
