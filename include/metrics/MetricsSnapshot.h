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

namespace mldp_pvxs_driver::metrics {

class Metrics;

/**
 * @brief Per-reader metrics data structure.
 */
struct ReaderMetrics
{
    std::string pv_name;             ///< PV identifier
    long long   pushes = 0;          ///< Total number of pushes
    double      bytes_total = 0.0;   ///< Total bytes transferred
    double      bytes_per_sec = 0.0; ///< Current transfer rate (bytes/second)
};

/**
 * @brief Per-writer snapshot data (distinct from WriterMetrics Prometheus class).
 */
struct WriterSnapshot
{
    std::string writer_name;              ///< Writer instance name
    long long   queue_depth = 0;          ///< Current queue depth
    long long   stream_rotations = 0;     ///< Total stream rotations
    long long   failures = 0;             ///< Total write failures
    double      payload_bytes_per_sec = 0.0; ///< Payload throughput (bytes/second)
    double      data_bytes_per_sec    = 0.0; ///< Raw data throughput (bytes/second)
    double      send_time_mean_ms     = 0.0; ///< Mean send latency (ms)
};

/**
 * @brief Connection pool metrics data structure.
 */
struct PoolMetrics
{
    long long in_use = 0;    ///< Connections currently in use
    long long available = 0; ///< Connections available in pool

    long long total() const
    {
        return in_use + available;
    }
};

/**
 * @brief Process-level CPU, memory, I/O, and thread metrics.
 */
struct ProcessMetrics
{
    double    cpu_user_ticks   = 0.0; ///< Cumulative user CPU ticks
    double    cpu_system_ticks = 0.0; ///< Cumulative system CPU ticks
    long long threads          = 0;   ///< Current thread count
    long long fds_open         = 0;   ///< Open file descriptors
    double    vm_size_bytes    = 0.0; ///< Virtual memory size (bytes)
    double    vm_rss_bytes     = 0.0; ///< Resident set size (bytes)
    double    vm_peak_bytes    = 0.0; ///< Peak virtual memory (bytes)
    double    rss_total_bytes  = 0.0; ///< Total RSS (anon+file+shmem, bytes)
    double    io_read_bytes    = 0.0; ///< Cumulative bytes read from storage
    double    io_write_bytes   = 0.0; ///< Cumulative bytes written to storage
};

/**
 * @brief Snapshot of driver metrics containing all collected data.
 */
struct MetricsData
{
    std::vector<ReaderMetrics> readers; ///< Per-reader statistics
    std::vector<WriterSnapshot> writers; ///< Per-writer statistics
    PoolMetrics                pool;    ///< Connection pool statistics
    ProcessMetrics             process; ///< Process-level CPU/memory/IO metrics
};

/**
 * @brief Takes a snapshot of driver metrics and provides formatting capabilities.
 *
 * This class is responsible for extracting metrics from a `Metrics` object,
 * structuring them into a `MetricsData` snapshot, and providing a static
 * method to convert the snapshot into a human-readable string format.
 */
class MetricsSnapshot
{
public:
    MetricsSnapshot() = default;
    ~MetricsSnapshot() = default;

    /**
     * @brief Extract and structure metrics into a snapshot.
     * @param metrics The metrics object containing the collected metrics.
     * @return Snapshot of structured metrics data.
     */
    MetricsData getSnapshot(const Metrics& metrics) const;

    /**
     * @brief Convert metrics snapshot to a human-readable string.
     * @param snapshot The metrics snapshot data.
     * @return Formatted metrics as a string.
     */
    static std::string toString(const MetricsData& snapshot);

private:
    /**
     * @brief Format bytes in human-readable units (B, KB, MB, GB).
     * @param bytes The number of bytes to format.
     * @return Formatted string with appropriate unit.
     */
    static std::string formatBytes(double bytes);

    /**
     * @brief Extract label value from a Prometheus metric line.
     * @param line The metric line to parse.
     * @param label The label name to extract (e.g., "reader=").
     * @return The label value, or empty string if not found.
     */
    static std::string extractLabelValue(std::string_view line, std::string_view label);

    /**
     * @brief Extract the numeric value from a Prometheus metric line.
     * @param line The metric line to parse.
     * @return The numeric value from the metric.
     */
    static double extractMetricValue(std::string_view line);

    /**
     * @brief Serialize metrics to Prometheus text exposition format.
     * @param metrics The metrics object to serialize.
     * @return Prometheus text format string.
     */
    static std::string serializeMetricsText(const Metrics& metrics);
};

} // namespace mldp_pvxs_driver::metrics
