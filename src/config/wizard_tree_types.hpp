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

// Plain-C++ types: no FTXUI dependency.
// Included by wizard_ui.hpp AND by tests that cannot link FTXUI.

#include <string>

namespace mldp_pvxs_driver::config::wizard_ui {

enum class TreeNodeKind
{
    Controller,
    WriterGroup,
    Writer,
    ReaderGroup,
    Reader,
    QueryableGroup,
    MetricsGroup,
    RoutingGroup
};

struct TreeNode
{
    TreeNodeKind kind;
    std::string  label;      // display text
    std::string  type_tag;   // "MLDP", "HDF5", "PVXS", "Base", "Arch", ""
    int          data_index; // index into respective WizardState vector (-1 for group nodes)
};

inline bool isGroupNode(TreeNodeKind k)
{
    return k == TreeNodeKind::WriterGroup ||
           k == TreeNodeKind::ReaderGroup ||
           k == TreeNodeKind::QueryableGroup ||
           k == TreeNodeKind::MetricsGroup ||
           k == TreeNodeKind::RoutingGroup;
}

} // namespace mldp_pvxs_driver::config::wizard_ui
