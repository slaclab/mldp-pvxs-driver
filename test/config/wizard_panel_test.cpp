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

// ── BuildTree HDF5 merge tag ──────────────────────────────────────────────────

TEST(WizardPanelBuildTree, Hdf5MergeTypeTag) {
    WizardState w;
    Hdf5WriterConfig hm; hm.name = "merge_w"; hm.is_merge = true;
    Hdf5WriterConfig hp; hp.name = "plain_w"; hp.is_merge = false;
    w.hdf5_writers = {hm, hp};

    auto tree = BuildTree(w);
    // Controller, WriterGroup, merge@0, plain@1, ReaderGroup, MetricsGroup, RoutingGroup
    ASSERT_EQ(7u, tree.size());
    EXPECT_EQ(tree[2].type_tag, "HDF5-merge");
    EXPECT_EQ(tree[3].type_tag, "HDF5");
    EXPECT_EQ(tree[2].data_index, 0);
    EXPECT_EQ(tree[3].data_index, 1);
}

// ── BuildTree last-item label uses └ ─────────────────────────────────────────

TEST(WizardPanelBuildTree, LastWriterUsesCornerChar) {
    WizardState w;
    MldpWriterConfig m0; m0.name = "w0";
    MldpWriterConfig m1; m1.name = "w1";
    w.mldp_writers = {m0, m1};

    auto tree = BuildTree(w);
    // index 3 is the last writer node
    EXPECT_NE(std::string::npos, tree[3].label.find("└"))
        << "Last writer label should use └, got: " << tree[3].label;
    // index 2 is not last
    EXPECT_EQ(std::string::npos, tree[2].label.find("└"))
        << "Non-last writer label should NOT use └, got: " << tree[2].label;
}

TEST(WizardPanelBuildTree, LastReaderUsesCornerChar) {
    WizardState w;
    EpicsReaderConfig r0; r0.name = "r0"; r0.reader_type = "epics-pvxs";
    EpicsReaderConfig r1; r1.name = "r1"; r1.reader_type = "epics-base";
    w.readers = {r0, r1};

    auto tree = BuildTree(w);
    // Controller, WriterGroup, ReaderGroup, Reader@0, Reader@1, MetricsGroup, RoutingGroup
    ASSERT_EQ(7u, tree.size());
    EXPECT_NE(std::string::npos, tree[4].label.find("└"))
        << "Last reader label should use └";
    EXPECT_EQ(std::string::npos, tree[3].label.find("└"))
        << "Non-last reader label should NOT use └";
}

// ── BuildTree add/delete sequence keeps indices stable ────────────────────────

TEST(WizardPanelBuildTree, IndicesAfterAddAndDelete) {
    WizardState w;
    MldpWriterConfig m0; m0.name = "keep_a";
    MldpWriterConfig m1; m1.name = "delete_me";
    MldpWriterConfig m2; m2.name = "keep_b";
    w.mldp_writers = {m0, m1, m2};

    // Delete middle
    w.mldp_writers.erase(w.mldp_writers.begin() + 1);
    auto tree = BuildTree(w);

    // Controller, WriterGroup, Writer@0, Writer@1, ReaderGroup, MetricsGroup, RoutingGroup
    ASSERT_EQ(7u, tree.size());
    EXPECT_EQ(tree[2].data_index, 0);
    EXPECT_EQ(tree[3].data_index, 1);
    // No stale index 2 or higher
    for (auto& n : tree)
        if (n.kind == TreeNodeKind::Writer)
            EXPECT_LT(n.data_index, 2);
}

TEST(WizardPanelBuildTree, MixedWriterIndependentIndices) {
    WizardState w;
    MldpWriterConfig m0; m0.name = "mldp_0";
    MldpWriterConfig m1; m1.name = "mldp_1";
    w.mldp_writers = {m0, m1};
    Hdf5WriterConfig h0; h0.name = "hdf5_0"; h0.is_merge = false;
    Hdf5WriterConfig h1; h1.name = "hdf5_1"; h1.is_merge = false;
    w.hdf5_writers = {h0, h1};

    auto tree = BuildTree(w);
    // Writer indices: MLDP@0, MLDP@1, HDF5@0, HDF5@1 — each type resets to 0
    EXPECT_EQ(tree[2].type_tag, "MLDP"); EXPECT_EQ(tree[2].data_index, 0);
    EXPECT_EQ(tree[3].type_tag, "MLDP"); EXPECT_EQ(tree[3].data_index, 1);
    EXPECT_EQ(tree[4].type_tag, "HDF5"); EXPECT_EQ(tree[4].data_index, 0);
    EXPECT_EQ(tree[5].type_tag, "HDF5"); EXPECT_EQ(tree[5].data_index, 1);
}

// ── BuildTree single-item groups omit ├ ──────────────────────────────────────

TEST(WizardPanelBuildTree, SingleWriterUsesCornerNotBranch) {
    WizardState w;
    MldpWriterConfig m; m.name = "only";
    w.mldp_writers = {m};

    auto tree = BuildTree(w);
    ASSERT_GE(tree.size(), 3u);
    EXPECT_NE(std::string::npos, tree[2].label.find("└"))
        << "Single writer should use └, got: " << tree[2].label;
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
