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
#include <memory>
#include <thread>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/labels.h>
#include <prometheus/registry.h>

#include <metrics/MetricsConfig.h>

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

    // Reader metrics ------------------------------------------------------
    void   incrementReaderEvents(double value = 1.0, prometheus::Labels tags = {});
    void   incrementReaderEventsReceived(double value = 1.0, prometheus::Labels tags = {});
    void   incrementReaderErrors(double value = 1.0, prometheus::Labels tags = {});
    void   observeReaderProcessingTimeMs(double value, prometheus::Labels tags = {});
    void   setReaderQueueDepth(double value, prometheus::Labels tags = {});
    void   setReaderPoolQueueDepth(double value, prometheus::Labels tags = {});
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

    // Bus metrics ---------------------------------------------------------
    void   incrementBusPushes(double value = 1.0, prometheus::Labels tags = {});
    void   incrementBusFailures(double value = 1.0, prometheus::Labels tags = {});
    void   incrementBusPayloadBytes(double value, prometheus::Labels tags = {});
    void   setBusPayloadBytesPerSecond(double value, prometheus::Labels tags = {});
    void   incrementBusStreamRotations(double value = 1.0, prometheus::Labels tags = {});
    double busPushTotal(prometheus::Labels tags = {}) const;
    double busFailuresTotal(prometheus::Labels tags = {}) const;
    double busPayloadBytesTotal(prometheus::Labels tags = {}) const;
    double busPayloadBytesPerSecond(prometheus::Labels tags = {}) const;

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

    prometheus::Family<prometheus::Gauge>* pool_connections_in_use_family_{nullptr};
    prometheus::Family<prometheus::Gauge>* pool_connections_available_family_{nullptr};

    prometheus::Histogram::BucketBoundaries    controller_send_time_buckets_;
    prometheus::Family<prometheus::Histogram>* controller_send_time_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     controller_queue_depth_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*     controller_channel_queue_depth_family_{nullptr};

    prometheus::Family<prometheus::Counter>* bus_push_family_{nullptr};
    prometheus::Family<prometheus::Counter>* bus_failure_family_{nullptr};
    prometheus::Family<prometheus::Counter>* bus_payload_bytes_family_{nullptr};
    prometheus::Family<prometheus::Gauge>*   bus_payload_bytes_per_second_family_{nullptr};
    prometheus::Family<prometheus::Counter>* bus_stream_rotations_family_{nullptr};

    std::unique_ptr<prometheus::Exposer> exposer_;

    // System metrics (via metric-grabber library) --------------------------------
    void startSystemMetricsCollection();
    void stopSystemMetricsCollection();
    void collectSystemMetricsLoop();

    std::atomic<bool> stop_system_metrics_{false};
    std::thread       system_metrics_thread_;

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
