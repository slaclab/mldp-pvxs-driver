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

#include <processor/InputBuffer.h>

#include <vector>

using mldp_pvxs_driver::processor::AlignmentPolicy;
using mldp_pvxs_driver::processor::InputBuffer;
using mldp_pvxs_driver::processor::TriggerPolicy;
using mldp_pvxs_driver::util::bus::BusTimestamp;
using mldp_pvxs_driver::util::bus::DataBatch;
using mldp_pvxs_driver::util::bus::DataColumn;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;
using mldp_pvxs_driver::util::bus::TimestampEntry;

namespace {

DataBatch makeFrame(uint64_t epoch_seconds, uint64_t nanoseconds, double value)
{
    DataBatch frame;
    frame.timestamps.push_back(TimestampEntry{epoch_seconds, nanoseconds});

    DataColumn column;
    column.name = "value";
    column.values = std::vector<double>{value};
    frame.columns.push_back(std::move(column));
    return frame;
}

TimeSeriesPayload makePayload(const std::vector<DataBatch>& frames)
{
    TimeSeriesPayload payload;
    payload.frames = frames;
    return payload;
}

double latestScalarValue(const DataBatch& batch)
{
    const auto& values = std::get<std::vector<double>>(batch.columns.at(0).values);
    return values.at(0);
}

} // namespace

TEST(InputBufferTest, AnyUpdateReturnsAfterFirstIngest)
{
    InputBuffer buffer({"pv:a"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(10, 1, 1.5)}));

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::AnyUpdate);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->channels.size(), 1u);
    EXPECT_EQ(snapshot->reference_time.epoch_seconds, 10u);
    EXPECT_EQ(snapshot->reference_time.nanoseconds, 1u);
}

TEST(InputBufferTest, AnyUpdateSnapshotContainsLatestValue)
{
    InputBuffer buffer({"pv:a"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(10, 1, 1.5)}));
    buffer.ingest("pv:a", makePayload({makeFrame(11, 2, 9.5)}));

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::AnyUpdate);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->channels.size(), 1u);
    EXPECT_DOUBLE_EQ(latestScalarValue(snapshot->channels.at("pv:a")), 9.5);
}

TEST(InputBufferTest, AllUpdatedNoSnapshotUntilAllFresh)
{
    InputBuffer buffer({"pv:a", "pv:b"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(10, 1, 1.0)}));

    EXPECT_FALSE(buffer.trySnapshot(TriggerPolicy::AllUpdated).has_value());
}

TEST(InputBufferTest, AllUpdatedSnapshotAfterBothFresh)
{
    InputBuffer buffer({"pv:a", "pv:b"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(10, 1, 1.0)}));
    buffer.ingest("pv:b", makePayload({makeFrame(12, 5, 2.0)}));

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::AllUpdated);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->channels.size(), 2u);
    EXPECT_EQ(snapshot->reference_time.epoch_seconds, 12u);
    EXPECT_EQ(snapshot->reference_time.nanoseconds, 5u);
}

TEST(InputBufferTest, AllUpdatedResetThenNoSnapshot)
{
    InputBuffer buffer({"pv:a", "pv:b"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(10, 1, 1.0)}));
    buffer.ingest("pv:b", makePayload({makeFrame(11, 1, 2.0)}));
    ASSERT_TRUE(buffer.trySnapshot(TriggerPolicy::AllUpdated).has_value());

    buffer.resetFreshFlags();
    buffer.ingest("pv:a", makePayload({makeFrame(12, 1, 3.0)}));

    EXPECT_FALSE(buffer.trySnapshot(TriggerPolicy::AllUpdated).has_value());
}

TEST(InputBufferTest, IntervalAlwaysReturnsSnapshot)
{
    InputBuffer buffer({"pv:a"}, AlignmentPolicy::LatestValue);

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::Interval);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->channels.empty());
    EXPECT_EQ(snapshot->reference_time.epoch_seconds, 0u);
    EXPECT_EQ(snapshot->reference_time.nanoseconds, 0u);
}

TEST(InputBufferTest, IntervalSnapshotContainsLatest)
{
    InputBuffer buffer({"pv:a"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(20, 7, 4.25)}));

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::Interval);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_DOUBLE_EQ(latestScalarValue(snapshot->channels.at("pv:a")), 4.25);
}

TEST(InputBufferTest, IgnoresUnknownSource)
{
    InputBuffer buffer({"pv:a"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:unknown", makePayload({makeFrame(10, 1, 1.0)}));

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::AnyUpdate);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->channels.empty());
}

TEST(InputBufferTest, EmptyPayloadFrames)
{
    InputBuffer buffer({"pv:a"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", TimeSeriesPayload{});

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::AllUpdated);
    EXPECT_FALSE(snapshot.has_value());
}

TEST(InputBufferTest, ReferenceTimeIsMax)
{
    InputBuffer buffer({"pv:a", "pv:b"}, AlignmentPolicy::LatestValue);
    buffer.ingest("pv:a", makePayload({makeFrame(100, 10, 1.0)}));
    buffer.ingest("pv:b", makePayload({makeFrame(100, 20, 2.0)}));

    const auto snapshot = buffer.trySnapshot(TriggerPolicy::AnyUpdate);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->reference_time.epoch_seconds, 100u);
    EXPECT_EQ(snapshot->reference_time.nanoseconds, 20u);
}
