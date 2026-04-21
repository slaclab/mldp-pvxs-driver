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

#ifdef MLDP_PVXS_HDF5_ENABLED

    #include <writer/hdf5/HDF5WriterMetrics.h>

    #include <prometheus/registry.h>

using namespace mldp_pvxs_driver::metrics;

// ---------------------------------------------------------------------------
// Fixture — fresh registry per test so counters start at zero.
// ---------------------------------------------------------------------------
class HDF5WriterMetricsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        registry_ = std::make_shared<prometheus::Registry>();
        metrics_  = std::make_unique<HDF5WriterMetrics>(*registry_, "test_ctrl", "test_writer");
    }

    std::shared_ptr<prometheus::Registry>  registry_;
    std::unique_ptr<HDF5WriterMetrics>     metrics_;
};

// ---------------------------------------------------------------------------
// Helper — extract a counter value from the registry by metric name.
// Returns -1.0 if not found.
// ---------------------------------------------------------------------------
static double getCounter(prometheus::Registry& registry,
                         const std::string&    name,
                         const prometheus::Labels& extra_labels = {})
{
    for (auto& family : registry.Collect())
    {
        if (family.name != name)
            continue;
        for (auto& metric : family.metric)
        {
            bool match = true;
            for (auto& [k, v] : extra_labels)
            {
                bool found = false;
                for (auto& lp : metric.label)
                {
                    if (lp.name == k && lp.value == v)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return metric.counter.value;
        }
    }
    return -1.0;
}

static double getGauge(prometheus::Registry& registry, const std::string& name)
{
    for (auto& family : registry.Collect())
    {
        if (family.name != name)
            continue;
        if (!family.metric.empty())
            return family.metric[0].gauge.value;
    }
    return -1.0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(HDF5WriterMetricsTest, BatchesWrittenIncrement)
{
    metrics_->incrementBatchesWritten();
    metrics_->incrementBatchesWritten();
    metrics_->incrementBatchesWritten(3.0);
    EXPECT_DOUBLE_EQ(5.0, getCounter(*registry_, "mldp_pvxs_driver_hdf5_batches_written_total"));
}

TEST_F(HDF5WriterMetricsTest, RowsWrittenPerSource)
{
    metrics_->incrementRowsWritten("PV:TEST:A", 10.0);
    metrics_->incrementRowsWritten("PV:TEST:A", 5.0);
    metrics_->incrementRowsWritten("PV:TEST:B", 7.0);

    EXPECT_DOUBLE_EQ(15.0, getCounter(*registry_, "mldp_pvxs_driver_hdf5_rows_written_total",
                                      {{"source", "PV:TEST:A"}}));
    EXPECT_DOUBLE_EQ(7.0,  getCounter(*registry_, "mldp_pvxs_driver_hdf5_rows_written_total",
                                      {{"source", "PV:TEST:B"}}));
}

TEST_F(HDF5WriterMetricsTest, BytesWrittenPerSource)
{
    metrics_->incrementBytesWritten("PV:TEST:A", 1024.0);
    metrics_->incrementBytesWritten("PV:TEST:A", 512.0);
    EXPECT_DOUBLE_EQ(1536.0, getCounter(*registry_, "mldp_pvxs_driver_hdf5_bytes_written_total",
                                         {{"source", "PV:TEST:A"}}));
}

TEST_F(HDF5WriterMetricsTest, QueueDepthGauge)
{
    metrics_->setQueueDepth(42.0);
    EXPECT_DOUBLE_EQ(42.0, getGauge(*registry_, "mldp_pvxs_driver_hdf5_queue_depth"));
    metrics_->setQueueDepth(0.0);
    EXPECT_DOUBLE_EQ(0.0, getGauge(*registry_, "mldp_pvxs_driver_hdf5_queue_depth"));
}

TEST_F(HDF5WriterMetricsTest, QueueDropsIncrement)
{
    metrics_->incrementQueueDrops();
    metrics_->incrementQueueDrops(4.0);
    EXPECT_DOUBLE_EQ(5.0, getCounter(*registry_, "mldp_pvxs_driver_hdf5_queue_drops_total"));
}

TEST_F(HDF5WriterMetricsTest, FileRotationsPerSource)
{
    metrics_->incrementFileRotations("PV:SRC:X");
    metrics_->incrementFileRotations("PV:SRC:X");
    metrics_->incrementFileRotations("PV:SRC:Y");
    EXPECT_DOUBLE_EQ(2.0, getCounter(*registry_, "mldp_pvxs_driver_hdf5_file_rotations_total",
                                      {{"source", "PV:SRC:X"}}));
    EXPECT_DOUBLE_EQ(1.0, getCounter(*registry_, "mldp_pvxs_driver_hdf5_file_rotations_total",
                                      {{"source", "PV:SRC:Y"}}));
}

TEST_F(HDF5WriterMetricsTest, WriteLatencyObserve)
{
    // Just verify no exception — histogram sample count is what we check.
    metrics_->observeWriteLatencyMs(0.5);
    metrics_->observeWriteLatencyMs(10.0);
    metrics_->observeWriteLatencyMs(250.0);

    bool found = false;
    for (auto& family : registry_->Collect())
    {
        if (family.name == "mldp_pvxs_driver_hdf5_write_latency_ms")
        {
            found = true;
            ASSERT_FALSE(family.metric.empty());
            EXPECT_EQ(3u, family.metric[0].histogram.sample_count);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(HDF5WriterMetricsTest, ConstantLabelsPresent)
{
    metrics_->incrementBatchesWritten();
    for (auto& family : registry_->Collect())
    {
        if (family.name == "mldp_pvxs_driver_hdf5_batches_written_total")
        {
            ASSERT_FALSE(family.metric.empty());
            bool found_ctrl   = false;
            bool found_writer = false;
            for (auto& lp : family.metric[0].label)
            {
                if (lp.name == "controller" && lp.value == "test_ctrl")
                    found_ctrl = true;
                if (lp.name == "writer" && lp.value == "test_writer")
                    found_writer = true;
            }
            EXPECT_TRUE(found_ctrl)   << "controller label missing";
            EXPECT_TRUE(found_writer) << "writer label missing";
        }
    }
}

#endif // MLDP_PVXS_HDF5_ENABLED
