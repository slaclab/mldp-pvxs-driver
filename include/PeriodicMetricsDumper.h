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

#include <metrics/Metrics.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

/**
 * @class PeriodicMetricsDumper
 * @brief Background thread manager for periodic metrics exports to a file.
 *
 * This class provides functionality to periodically collect and persist Prometheus metrics
 * to a file in JSON Lines (JSONL) format. Each entry is a complete JSON object containing:
 * - `timestamp_ms`: Milliseconds since epoch (machine-readable)
 * - `timestamp_iso`: ISO 8601 formatted timestamp (human-readable)
 * - `metrics`: All collected Prometheus metrics as key-value pairs
 *
 * The dumping happens in a background thread at a configurable interval, ensuring
 * non-blocking operation of the main application. Thread safety is ensured through
 * mutex protection on file I/O operations.
 *
 * @note This class is non-copyable and must be moved or managed via smart pointers.
 * @note The Metrics reference must remain valid for the lifetime of this object.
 *
 * @example
 * @code
 * // Create and start periodic dumper
 * auto dumper = std::make_unique<PeriodicMetricsDumper>(
 *     controller.metrics(),
 *     "metrics.jsonl",
 *     std::chrono::seconds(60)
 * );
 *
 * // Dumper starts background thread
 * dumper->start();
 *
 * // ... application runs ...
 *
 * // Stop dumper when done (automatic via destructor)
 * dumper->stop();
 * @endcode
 */
class PeriodicMetricsDumper
{
private:
    friend class PeriodicMetricsDumperTest;

    std::thread                         dump_thread;        ///< Background thread for periodic dumps
    volatile std::atomic<bool>          should_stop{false}; ///< Flag to signal thread shutdown
    std::mutex                          state_mutex;        ///< Protects shared state and condition variable
    std::condition_variable             stop_signal;        ///< Wakes thread immediately on stop request
    std::mutex                          output_path_mutex;  ///< Protects output_path access
    std::string                         output_path;        ///< File path for metric exports
    std::chrono::milliseconds           interval;           ///< Dump interval in milliseconds
    mldp_pvxs_driver::metrics::Metrics& metrics;            ///< Reference to metrics registry

    /**
     * @brief Internal thread function for periodic metric dumping.
     *
     * Runs in background thread, sleeping for the specified interval between dumps.
     * Calls appendMetricsToFile() at each interval or until should_stop is set.
     *
     * @thread Runs in background thread spawned by start()
     */
    void start();

    /**
     * @brief Internal stop function that joins the background thread.
     *
     * Signals the condition variable to wake up the sleeping thread immediately,
     * allowing graceful shutdown without waiting for the full sleep interval to elapse.
     */
    void stop();

public:
    /// Explicitly deleted default constructor
    PeriodicMetricsDumper() = delete;

    /**
     * @brief Construct a periodic metrics dumper.
     *
     * Initializes the dumper with the metrics registry to monitor, output file path,
     * and dump interval. The background thread is not started until start() is called.
     *
     * @param metrics Reference to the Metrics registry to dump periodically.
     *                Must remain valid for the lifetime of this object.
     * @param path Output file path for metrics in JSON Lines format.
     *             File is created if it doesn't exist; appended to if it does.
     * @param dump_interval Interval between periodic dumps in milliseconds.
     *                      Should typically be in the range of seconds to minutes.
     *
     * @example
     * @code
     * auto dumper = std::make_unique<PeriodicMetricsDumper>(
     *     controller.metrics(),
     *     "metrics.jsonl",
     *     std::chrono::seconds(30)
     * );
     * dumper->start();
     * @endcode
     */
    explicit PeriodicMetricsDumper(mldp_pvxs_driver::metrics::Metrics& metrics,
                                   const std::string&                  path,
                                   std::chrono::milliseconds           dump_interval);

    /**
     * @brief Destructor.
     *
     * Ensures the background thread is stopped gracefully before the object is destroyed.
     * If stop() hasn't been called explicitly, it will be called in the destructor.
     */
    ~PeriodicMetricsDumper();

    /**
     * @brief Serialize current metrics to JSON Lines format.
     *
     * Converts metrics from Prometheus text exposition format to a single-line
     * JSON object with timestamp and metric values.
     *
     * @return std::string JSON Lines formatted metrics with ISO 8601 and millisecond timestamps
     *
     * @throws None (errors logged internally)
     */
    std::string serializeMetricsJsonl();

    /**
     * @brief Append serialized metrics to output file.
     *
     * Opens the file in append mode and writes the JSON Lines formatted metrics.
     * File errors are caught and logged via spdlog without propagating exceptions.
     *
     * @throws None (all exceptions caught and logged)
     */
    void appendMetricsToFile();
};
