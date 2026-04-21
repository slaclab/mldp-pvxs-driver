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

#include <metrics/WriterMetrics.h>

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <string>

namespace mldp_pvxs_driver::metrics {

/**
 * @brief Prometheus metric families for the HDF5 writer.
 *
 * All families share constant labels `{controller=<name>, writer=<instance>}`.
 * Per-source counters accept an additional `source` label at call time, following
 * the same pattern as the existing reader metrics in `Metrics`.
 *
 * Registered metric names:
 * | Name | Type | Extra label |
 * |------|------|-------------|
 * | mldp_pvxs_driver_hdf5_batches_written_total  | Counter   | —       |
 * | mldp_pvxs_driver_hdf5_rows_written_total     | Counter   | source  |
 * | mldp_pvxs_driver_hdf5_bytes_written_total    | Counter   | source  |
 * | mldp_pvxs_driver_hdf5_queue_depth            | Gauge     | —       |
 * | mldp_pvxs_driver_hdf5_queue_drops_total      | Counter   | —       |
 * | mldp_pvxs_driver_hdf5_file_rotations_total   | Counter   | source  |
 * | mldp_pvxs_driver_hdf5_write_latency_ms       | Histogram | —       |
 */
class HDF5WriterMetrics : public WriterMetrics
{
public:
    /**
     * @brief Register all HDF5 metric families into @p registry.
     *
     * @param registry        Shared Prometheus registry (from Metrics::registry()).
     * @param controller_name Value for the constant `controller` label.
     * @param writer_name     Value for the constant `writer` label.
     */
    HDF5WriterMetrics(prometheus::Registry& registry,
                      const std::string&    controller_name,
                      const std::string&    writer_name);

    ~HDF5WriterMetrics() override = default;

    // -----------------------------------------------------------------------
    // Writer-level metrics (no source label)
    // -----------------------------------------------------------------------

    /** Increment the count of EventBatches written to HDF5. */
    void incrementBatchesWritten(double value = 1.0);

    /** Set the current write-queue depth (number of pending QueueEntries). */
    void setQueueDepth(double value);

    /** Increment the count of batches dropped due to queue overflow. */
    void incrementQueueDrops(double value = 1.0);

    /**
     * @brief Record one write-latency sample (appendFrame / flushTabularBuffer).
     * @param ms Elapsed time in milliseconds.
     */
    void observeWriteLatencyMs(double ms);

    // -----------------------------------------------------------------------
    // Per-source metrics (source label added at call time)
    // -----------------------------------------------------------------------

    /** Increment the count of rows (samples) written for @p source. */
    void incrementRowsWritten(const std::string& source, double value = 1.0);

    /** Increment the byte counter for @p source. */
    void incrementBytesWritten(const std::string& source, double value);

    /** Increment the file-rotation counter for @p source. */
    void incrementFileRotations(const std::string& source, double value = 1.0);

private:
    // Writer-level (constant labels only)
    prometheus::Histogram::BucketBoundaries    write_latency_ms_buckets_;
    prometheus::Family<prometheus::Counter>*   batches_written_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     queue_depth_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   queue_drops_family_{nullptr};
    prometheus::Family<prometheus::Histogram>* write_latency_ms_family_{nullptr};

    // Per-source (constant labels + dynamic source label)
    prometheus::Family<prometheus::Counter>* rows_written_family_{nullptr};
    prometheus::Family<prometheus::Counter>* bytes_written_family_{nullptr};
    prometheus::Family<prometheus::Counter>* file_rotations_family_{nullptr};
};

} // namespace mldp_pvxs_driver::metrics
