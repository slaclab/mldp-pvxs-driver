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

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/labels.h>
#include <prometheus/registry.h>

#include <metrics/MetricsConfig.h>
#include <metrics/procmon/MetricsSnapshot.hpp>

// Forward declaration for metric-grabber
namespace procmon {
class MetricsCollector;
}

namespace mldp_pvxs_driver::metrics {

/**
 * @brief Metrics collector that exposes counters and gauges for the driver.
 *
 * The collector groups metrics around the three main areas of the driver:
 * - Readers: track events flowing from EPICS into the system.
 * - Pool: monitor connection pool pressure.
 * - Bus: track pushes forwarded to MLDP.
 */
class Metrics
{
public:
    /** @brief Construct a collector with an optional metrics configuration. */
    Metrics() = delete;
    explicit Metrics(const MetricsConfig& config, std::string controller_name = "default");
    ~Metrics();

    /** @return Registry that can be scraped/exported by HTTP exposers. */
    std::shared_ptr<prometheus::Registry> registry() const;

    /** @return Controller name stamped on all metric labels. */
    const std::string& controllerName() const;

    // Reader metrics ------------------------------------------------------
    void   incrementReaderEvents(double value = 1.0, prometheus::Labels tags = {});
    void   incrementReaderEventsReceived(double value = 1.0, prometheus::Labels tags = {});
    void   incrementReaderErrors(double value = 1.0, prometheus::Labels tags = {});
    void   observeReaderProcessingTimeMs(double value, prometheus::Labels tags = {});
    void   setReaderQueueDepth(double value, prometheus::Labels tags = {});
    void   setReaderPoolQueueDepth(double value, prometheus::Labels tags = {});
    void   incrementReaderDataBytesTotal(double value, prometheus::Labels tags = {});
    void   setReaderDataBytesPerSecond(double value, prometheus::Labels tags = {});
    double readerDataBytesTotal(prometheus::Labels tags = {}) const;
    double readerDataBytesPerSecond(prometheus::Labels tags = {}) const;
    double readerEventsTotal() const;
    double readerEventsReceivedTotal() const;
    double readerErrorsTotal() const;

    // Pool metrics --------------------------------------------------------
    /**
     * @brief Gauge for connections currently checked out of the pool.
     */
    void setPoolConnectionsInUse(double value, prometheus::Labels tags = {});
    /**
     * @brief Gauge for idle connections available in the pool.
     */
    void   setPoolConnectionsAvailable(double value, prometheus::Labels tags = {});
    double poolConnectionsInUse(prometheus::Labels tags = {}) const;
    double poolConnectionsAvailable(prometheus::Labels tags = {}) const;

    // Controller metrics -------------------------------------------------
    void observeControllerSendTimeSeconds(double value, prometheus::Labels tags = {});
    void setControllerQueueDepth(double value, prometheus::Labels tags = {});
    void setControllerChannelQueueDepth(double value, prometheus::Labels tags = {});

    // Processor metrics --------------------------------------------------
    void observeProcessorComputeLatencyUs(double value, prometheus::Labels tags = {});
    void incrementProcessorFireCount(double value = 1.0, prometheus::Labels tags = {});
    void setProcessorBufferDepth(double value, prometheus::Labels tags = {});
    void incrementProcessorComputeErrors(double value = 1.0, prometheus::Labels tags = {});
    void incrementProcessorSnapshotMisses(double value = 1.0, prometheus::Labels tags = {});

    // Writer metrics ---------------------------------------------------------
    void   incrementWriterPushes(double value = 1.0, prometheus::Labels tags = {});
    void   incrementWriterFailures(double value = 1.0, prometheus::Labels tags = {});
    void   incrementWriterPayloadBytes(double value, prometheus::Labels tags = {});
    void   setWriterPayloadBytesPerSecond(double value, prometheus::Labels tags = {});
    void   incrementWriterStreamRotations(double value = 1.0, prometheus::Labels tags = {});
    double writerPushTotal(prometheus::Labels tags = {}) const;
    double writerFailuresTotal(prometheus::Labels tags = {}) const;
    double writerPayloadBytesTotal(prometheus::Labels tags = {}) const;
    double writerPayloadBytesPerSecond(prometheus::Labels tags = {}) const;
    void   incrementWriterDataBytesTotal(double value, prometheus::Labels tags = {});
    void   setWriterDataBytesPerSecond(double value, prometheus::Labels tags = {});
    double writerDataBytesTotal(prometheus::Labels tags = {}) const;
    double writerDataBytesPerSecond(prometheus::Labels tags = {}) const;
    void   setWriterPostConvDataBytesPerSecond(double value, prometheus::Labels tags = {});
    double writerPostConvDataBytesPerSecond(prometheus::Labels tags = {}) const;

private:
    MetricsConfig                         config_;
    std::string                           controller_name_;
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Histogram::BucketBoundaries reader_processing_time_ms_buckets_;

    prometheus::Family<prometheus::Counter>*   reader_events_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   reader_events_received_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   reader_errors_family_{nullptr};
    prometheus::Family<prometheus::Histogram>* reader_processing_time_ms_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     reader_queue_depth_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     reader_pool_queue_depth_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   reader_data_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     reader_data_bytes_per_second_family_{nullptr};

    prometheus::Family<prometheus::Gauge>* pool_connections_in_use_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* pool_connections_available_family_{nullptr};

    prometheus::Histogram::BucketBoundaries    controller_send_time_buckets_;
    prometheus::Family<prometheus::Histogram>* controller_send_time_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     controller_queue_depth_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     controller_channel_queue_depth_family_{nullptr};

    prometheus::Histogram::BucketBoundaries    processor_compute_latency_us_buckets_;
    prometheus::Family<prometheus::Histogram>* processor_compute_latency_us_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   processor_fire_count_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     processor_buffer_depth_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   processor_compute_errors_family_{nullptr};
    prometheus::Family<prometheus::Counter>*   processor_snapshot_misses_family_{nullptr};

    prometheus::Family<prometheus::Counter>* writer_push_family_{nullptr};
    prometheus::Family<prometheus::Counter>* writer_failure_family_{nullptr};
    prometheus::Family<prometheus::Counter>* writer_payload_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*   writer_payload_bytes_per_second_family_{nullptr};
    prometheus::Family<prometheus::Counter>* writer_stream_rotations_family_{nullptr};

    prometheus::Family<prometheus::Counter>* writer_data_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*   writer_data_bytes_per_second_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*   writer_post_conv_data_bytes_per_second_family_{nullptr};

    std::unique_ptr<prometheus::Exposer> exposer_;

    // System metrics (via metric-grabber library) --------------------------------
    void startSystemMetricsCollection();
    void stopSystemMetricsCollection();
    void collectSystemMetricsLoop();

    std::atomic<bool>       stop_system_metrics_{false};
    std::condition_variable stop_metrics_cv_;
    std::mutex              stop_metrics_mutex_;
    std::thread             system_metrics_thread_;

    // CPU metrics (counters - values accumulate over time)
    prometheus::Family<prometheus::Counter>* process_cpu_user_ticks_family_{nullptr};
    prometheus::Family<prometheus::Counter>* process_cpu_system_ticks_family_{nullptr};
    prometheus::Family<prometheus::Counter>* process_cpu_children_user_ticks_family_{nullptr};
    prometheus::Family<prometheus::Counter>* process_cpu_children_system_ticks_family_{nullptr};

    // Memory metrics (gauges - current values)
    prometheus::Family<prometheus::Gauge>* process_memory_virtual_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_memory_rss_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_memory_virtual_peak_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_memory_rss_anon_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_memory_rss_file_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_memory_rss_shmem_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_memory_rss_total_bytes_family_{nullptr};

    // I/O metrics (counters - bytes accumulate over time)
    prometheus::Family<prometheus::Counter>* process_io_read_bytes_family_{nullptr};
    prometheus::Family<prometheus::Counter>* process_io_write_bytes_family_{nullptr};
    prometheus::Family<prometheus::Counter>* process_io_cancelled_write_bytes_family_{nullptr};

    // Context switches (counters)
    prometheus::Family<prometheus::Counter>* process_context_switches_voluntary_family_{nullptr};
    prometheus::Family<prometheus::Counter>* process_context_switches_involuntary_family_{nullptr};

    // File descriptors (gauge)
    prometheus::Family<prometheus::Gauge>* process_fds_open_family_{nullptr};

    // Thread count (gauge)
    prometheus::Family<prometheus::Gauge>* process_threads_family_{nullptr};

    // Process info
    prometheus::Family<prometheus::Gauge>* process_priority_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* process_nice_family_{nullptr};

    // Metric grabber collector
    std::unique_ptr<procmon::MetricsCollector> system_metrics_collector_;

    // Previous snapshot for delta computation of cumulative counters
    std::optional<procmon::MetricsSnapshot> prev_system_snapshot_;
};

// Helper to safely call a metrics method when the pointer may be null.
template <class MetricsPtr, class Fn>
inline void metric_call(MetricsPtr&& metrics, Fn&& fn)
{
    if (metrics)
    {
        fn(*metrics);
    }
}

} // namespace mldp_pvxs_driver::metrics
