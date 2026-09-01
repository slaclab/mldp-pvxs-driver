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
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

struct EditListOptions
{
    std::string path = "config.yaml";
};

struct EditRemoveOptions
{
    std::string path = "config.yaml";
    std::string kind; // "writer", "reader", "routing"
    std::string name;
    bool        no_backup = false;
    bool        dry_run = false;
};

int runList(const EditListOptions& opts);
int runRemove(const EditRemoveOptions& opts);

#ifdef MLDP_WIZARD_ENABLED
// Interactive add path — launches FTXUI sub-flow for the given kind.
int runAddInteractive(const std::string& path, const std::string& kind, bool no_backup, bool dry_run);
#endif

} // namespace mldp_pvxs_driver::config
