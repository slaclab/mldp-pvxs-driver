//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>
#include <controller/RouteTable.h>

namespace mldp_pvxs_driver::controller {

// 1. EmptyConfig_IsAllToAll — default-constructed RouteTable isAllToAll()==true, accepts any pair
TEST(RouteTableTest, EmptyConfig_IsAllToAll) {
    RouteTable rt;
    EXPECT_TRUE(rt.isAllToAll());
    EXPECT_TRUE(rt.accepts("any_writer", "any_reader"));
}

// 2. Build_EmptyRoutes_IsAllToAll — build with empty routes vector → still all-to-all
TEST(RouteTableTest, Build_EmptyRoutes_IsAllToAll) {
    RouteTable rt = RouteTable::build({}, {"r1"}, {"w1"});
    EXPECT_TRUE(rt.isAllToAll());
    EXPECT_TRUE(rt.accepts("w1", "r1"));
}

// 3. ConfiguredRoutes_FilterCorrectly — w1 accepts r1 only, w2 accepts r2 only
TEST(RouteTableTest, ConfiguredRoutes_FilterCorrectly) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"r1"}}, {"w2", {"r2"}}},
        {"r1", "r2"}, {"w1", "w2"});
    EXPECT_FALSE(rt.isAllToAll());
    EXPECT_TRUE(rt.accepts("w1", "r1"));
    EXPECT_FALSE(rt.accepts("w1", "r2"));
    EXPECT_TRUE(rt.accepts("w2", "r2"));
    EXPECT_FALSE(rt.accepts("w2", "r1"));
}

// 4. AcceptAll_Keyword — writer with from:[all] accepts any reader
TEST(RouteTableTest, AcceptAll_Keyword) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"all"}}},
        {"r1", "r2"}, {"w1"});
    EXPECT_FALSE(rt.isAllToAll());
    EXPECT_TRUE(rt.accepts("w1", "r1"));
    EXPECT_TRUE(rt.accepts("w1", "r2"));
    EXPECT_TRUE(rt.accepts("w1", "unknown_reader"));
}

// 5. UnknownReaderName_Throws
TEST(RouteTableTest, UnknownReaderName_Throws) {
    EXPECT_THROW(
        RouteTable::build({{"w1", {"nonexistent"}}}, {"r1"}, {"w1"}),
        std::runtime_error);
}

// 6. UnknownWriterName_Throws
TEST(RouteTableTest, UnknownWriterName_Throws) {
    EXPECT_THROW(
        RouteTable::build({{"nonexistent", {"r1"}}}, {"r1"}, {"w1"}),
        std::runtime_error);
}

// 7. WriterNotInRouting_RejectsAll — when routing configured, writer not mentioned gets nothing
TEST(RouteTableTest, WriterNotInRouting_RejectsAll) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"r1"}}},
        {"r1"}, {"w1", "w2"});
    EXPECT_FALSE(rt.accepts("w2", "r1"));
}

// 8. OrphanReaders_Detected
TEST(RouteTableTest, OrphanReaders_Detected) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"r1"}}},
        {"r1", "r2"}, {"w1"});
    auto orphans = rt.orphanReaders({"r1", "r2"});
    ASSERT_EQ(1u, orphans.size());
    EXPECT_EQ("r2", orphans[0]);
}

// 9. OrphanWriters_Detected
TEST(RouteTableTest, OrphanWriters_Detected) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"r1"}}},
        {"r1"}, {"w1", "w2"});
    auto orphans = rt.orphanWriters({"w1", "w2"});
    ASSERT_EQ(1u, orphans.size());
    EXPECT_EQ("w2", orphans[0]);
}

// 10. DuplicateReader_Handled — duplicate reader in from list doesn't cause issues
TEST(RouteTableTest, DuplicateReader_Handled) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"r1", "r1"}}},
        {"r1"}, {"w1"});
    EXPECT_TRUE(rt.accepts("w1", "r1"));
}

// 11. ManyToMany — one reader feeds multiple writers
TEST(RouteTableTest, ManyToMany_OneReaderMultipleWriters) {
    RouteTable rt = RouteTable::build(
        {{"w1", {"r1", "r2"}}, {"w2", {"r1"}}},
        {"r1", "r2"}, {"w1", "w2"});
    EXPECT_TRUE(rt.accepts("w1", "r1"));
    EXPECT_TRUE(rt.accepts("w1", "r2"));
    EXPECT_TRUE(rt.accepts("w2", "r1"));
    EXPECT_FALSE(rt.accepts("w2", "r2"));
}

// --- acceptsSource tests ---

// acceptsSource — no patterns, all-to-all → always true
TEST(RouteTableTest, AcceptsSource_AllToAll_AlwaysTrue) {
    RouteTable rt; // default = all_to_all
    EXPECT_TRUE(rt.acceptsSource("any_writer", "SYSTEM:SENSOR:PV1"));
    EXPECT_TRUE(rt.acceptsSource("any_writer", ""));
}

// acceptsSource — no patterns on route → all sources pass
TEST(RouteTableTest, AcceptsSource_NoPatterns_PassAll) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_TRUE(rt.acceptsSource("w1", "SYSTEM:SENSOR:PV1"));
    EXPECT_TRUE(rt.acceptsSource("w1", "ANYTHING"));
}

// acceptsSource — include pattern: source matching include → passes
TEST(RouteTableTest, AcceptsSource_Include_MatchPasses) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    e.include_patterns = {"SYSTEM:SENSOR:*"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_TRUE(rt.acceptsSource("w1", "SYSTEM:SENSOR:PV1"));
    EXPECT_TRUE(rt.acceptsSource("w1", "SYSTEM:SENSOR:PV2"));
}

// acceptsSource — include pattern: source NOT matching include → dropped
TEST(RouteTableTest, AcceptsSource_Include_NoMatchDropped) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    e.include_patterns = {"SYSTEM:SENSOR:*"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_FALSE(rt.acceptsSource("w1", "OTHER:SOURCE:PV1"));
    EXPECT_FALSE(rt.acceptsSource("w1", ""));
}

// acceptsSource — exclude wins over include match
TEST(RouteTableTest, AcceptsSource_ExcludeWins) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    e.include_patterns = {"SYSTEM:*"};
    e.exclude_patterns = {"SYSTEM:TEST:*"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_TRUE(rt.acceptsSource("w1", "SYSTEM:SENSOR:PV1"));
    EXPECT_FALSE(rt.acceptsSource("w1", "SYSTEM:TEST:PV1"));
}

// acceptsSource — exclude only, no include: source matching exclude → dropped
TEST(RouteTableTest, AcceptsSource_ExcludeOnly_DropMatch) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    e.exclude_patterns = {"BAD:*"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_TRUE(rt.acceptsSource("w1", "GOOD:PV1"));
    EXPECT_FALSE(rt.acceptsSource("w1", "BAD:PV1"));
}

// acceptsSource — glob ? matches single char
TEST(RouteTableTest, AcceptsSource_GlobQuestionMark) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    e.include_patterns = {"SRC:??"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_TRUE(rt.acceptsSource("w1", "SRC:AB"));
    EXPECT_FALSE(rt.acceptsSource("w1", "SRC:ABC")); // 3 chars, not 2
    EXPECT_FALSE(rt.acceptsSource("w1", "SRC:A"));   // 1 char, not 2
}

// acceptsSource — writer not in routing when routing configured → false
TEST(RouteTableTest, AcceptsSource_UnknownWriter_ReturnsFalse) {
    RouteFilterEntry e;
    e.writer_name = "w1";
    e.from_readers = {"r1"};
    RouteTable rt = RouteTable::build({e}, {"r1"}, {"w1"});
    EXPECT_FALSE(rt.acceptsSource("w2", "SYSTEM:PV1"));
}

} // namespace mldp_pvxs_driver::controller
