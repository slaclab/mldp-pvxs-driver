//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#ifdef MLDP_WIZARD_ENABLED

#include <gtest/gtest.h>
#include <config/wizard.h>
#include "wizard_internal.h"
#include "wizard_tree_types.hpp"

namespace mldp_pvxs_driver::config {

using namespace wizard_ui;
std::vector<TreeNode> BuildTree(const WizardState& w);
std::string GetHelpText(wizard_ui::TreeNodeKind kind, const std::string& field);

} // namespace mldp_pvxs_driver::config

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::config::wizard_ui;

// ── BuildTreeControllerOnly ──────────────────────────────────────────────────

TEST(WizardPanelBuildTree, ControllerOnly) {
    WizardState w;
    auto tree = BuildTree(w);
    // Expect exactly: Controller, WriterGroup, ReaderGroup, MetricsGroup, RoutingGroup
    ASSERT_EQ(tree.size(), 5u);
    EXPECT_EQ(tree[0].kind, TreeNodeKind::Controller);
    EXPECT_EQ(tree[1].kind, TreeNodeKind::WriterGroup);
    EXPECT_EQ(tree[2].kind, TreeNodeKind::ReaderGroup);
    EXPECT_EQ(tree[3].kind, TreeNodeKind::MetricsGroup);
    EXPECT_EQ(tree[4].kind, TreeNodeKind::RoutingGroup);
    // All group nodes should have data_index == -1
    for (auto& n : tree) EXPECT_EQ(n.data_index, -1);
}

// ── BuildTreeWithEntries ─────────────────────────────────────────────────────

TEST(WizardPanelBuildTree, WithEntries) {
    WizardState w;
    MldpWriterConfig m1; m1.name = "mldp_a";
    MldpWriterConfig m2; m2.name = "mldp_b";
    Hdf5WriterConfig h1; h1.name = "hdf5_x"; h1.is_merge = false;
    w.mldp_writers = {m1, m2};
    w.hdf5_writers = {h1};
    EpicsReaderConfig r1; r1.name = "pvxs_0"; r1.reader_type = "epics-pvxs";
    EpicsReaderConfig r2; r2.name = "base_0"; r2.reader_type = "epics-base";
    w.readers = {r1, r2};

    auto tree = BuildTree(w);

    // Expected layout: Controller, WriterGroup, MLDP@0, MLDP@1, HDF5@0,
    //                  ReaderGroup, Reader@0, Reader@1, MetricsGroup, RoutingGroup
    ASSERT_EQ(tree.size(), 10u);

    EXPECT_EQ(tree[0].kind, TreeNodeKind::Controller);
    EXPECT_EQ(tree[1].kind, TreeNodeKind::WriterGroup);

    EXPECT_EQ(tree[2].kind, TreeNodeKind::Writer);
    EXPECT_EQ(tree[2].type_tag, "MLDP");
    EXPECT_EQ(tree[2].data_index, 0);

    EXPECT_EQ(tree[3].kind, TreeNodeKind::Writer);
    EXPECT_EQ(tree[3].type_tag, "MLDP");
    EXPECT_EQ(tree[3].data_index, 1);

    EXPECT_EQ(tree[4].kind, TreeNodeKind::Writer);
    EXPECT_EQ(tree[4].type_tag, "HDF5");
    EXPECT_EQ(tree[4].data_index, 0);

    EXPECT_EQ(tree[5].kind, TreeNodeKind::ReaderGroup);

    EXPECT_EQ(tree[6].kind, TreeNodeKind::Reader);
    EXPECT_EQ(tree[6].data_index, 0);

    EXPECT_EQ(tree[7].kind, TreeNodeKind::Reader);
    EXPECT_EQ(tree[7].data_index, 1);

    EXPECT_EQ(tree[8].kind, TreeNodeKind::MetricsGroup);
    EXPECT_EQ(tree[9].kind, TreeNodeKind::RoutingGroup);
}

// ── BuildTreeAfterDelete ─────────────────────────────────────────────────────

TEST(WizardPanelBuildTree, AfterDelete) {
    WizardState w;
    MldpWriterConfig m0; m0.name = "keep";
    MldpWriterConfig m1; m1.name = "delete_me";
    w.mldp_writers = {m0, m1};

    // Delete index 1
    w.mldp_writers.erase(w.mldp_writers.begin() + 1);
    auto tree = BuildTree(w);

    // Controller, WriterGroup, Writer@0, ReaderGroup, MetricsGroup, RoutingGroup
    ASSERT_EQ(tree.size(), 6u);
    EXPECT_EQ(tree[2].kind, TreeNodeKind::Writer);
    EXPECT_EQ(tree[2].data_index, 0);
    // data_index 1 should NOT appear anywhere
    for (auto& n : tree) {
        if (n.kind == TreeNodeKind::Writer)
            EXPECT_LT(n.data_index, 1);
    }
}

// ── HelpTextCoverage ─────────────────────────────────────────────────────────

TEST(WizardPanelHelp, TextCoverage) {
    static const std::vector<TreeNodeKind> all_kinds = {
        TreeNodeKind::Controller,
        TreeNodeKind::Writer,
        TreeNodeKind::Reader,
        TreeNodeKind::MetricsGroup,
        TreeNodeKind::RoutingGroup,
    };
    for (auto kind : all_kinds) {
        // GetHelpText falls back to first entry for the kind if field not found
        std::string help = GetHelpText(kind, "name");
        EXPECT_FALSE(help.empty()) << "No help text for kind " << static_cast<int>(kind);
    }
}

#endif // MLDP_WIZARD_ENABLED
