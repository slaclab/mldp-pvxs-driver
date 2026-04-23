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
 * @brief Snapshot of driver metrics containing all collected data.
 */
struct MetricsData
{
    std::vector<ReaderMetrics> readers; ///< Per-reader statistics
    PoolMetrics                pool;    ///< Connection pool statistics
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
