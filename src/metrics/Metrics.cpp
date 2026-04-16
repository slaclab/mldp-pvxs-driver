//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <metrics/Metrics.h>
#include <metricsgrabber/MetricsGrabber.hpp>
#include <util/log/Logger.h>

#include <unistd.h>
#include <utility>

namespace {

prometheus::Family<prometheus::Counter>&
makeCounterFamily(prometheus::Registry& registry, std::string name, std::string help)
{
    return prometheus::BuildCounter()
        .Name(std::move(name))
        .Help(std::move(help))
        .Register(registry);
}

prometheus::Family<prometheus::Gauge>&
makeGaugeFamily(prometheus::Registry& registry, std::string name, std::string help)
{
    return prometheus::BuildGauge()
        .Name(std::move(name))
        .Help(std::move(help))
        .Register(registry);
}

prometheus::Family<prometheus::Histogram>&
makeHistogramFamily(prometheus::Registry& registry, std::string name, std::string help)
{
    return prometheus::BuildHistogram()
        .Name(std::move(name))
        .Help(std::move(help))
        .Register(registry);
}

} // namespace

using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::util::log;

Metrics::Metrics(const MetricsConfig& config)
    : config_(config)
    , registry_(std::make_shared<prometheus::Registry>())
    // Reader metrics
    , reader_events_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_reader_events_total", "Total events processed by readers."))
    , reader_events_received_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_reader_events_received_total", "Total raw EPICS updates received from subscriptions before processing."))
    , reader_errors_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_reader_errors_total", "Total reader failures observed when pulling data."))
    , reader_processing_time_ms_buckets_({0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0})
    , reader_processing_time_ms_family_(makeHistogramFamily(
          *registry_,
          "mldp_pvxs_driver_reader_processing_time_ms",
          "Time spent converting EPICS PV updates to MLDP protobuf payloads (milliseconds)."))
    , reader_queue_depth_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_reader_queue_depth", "Number of PV updates queued in the reader work queue awaiting processing."))
    , reader_pool_queue_depth_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_reader_pool_queue_depth", "Number of conversion tasks queued in the reader thread pool awaiting processing."))
    // Pool metrics
    , pool_connections_in_use_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_pool_connections_in_use", "Number of MLDP gRPC connections currently in use."))
    , pool_connections_available_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_pool_connections_available", "Idle MLDP gRPC connections ready for use."))
    // Controller metrics
    , controller_send_time_buckets_({0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0})
    , controller_send_time_family_(makeHistogramFamily(*registry_,
                                                       "mldp_pvxs_driver_controller_send_time_seconds",
                                                       "Time spent sending event batches to MLDP (seconds)."))
    , controller_queue_depth_family_(makeGaugeFamily(*registry_,
                                                     "mldp_pvxs_driver_controller_queue_depth",
                                                     "Number of queued controller tasks waiting to send batches."))
    , controller_channel_queue_depth_family_(makeGaugeFamily(*registry_,
                                                             "mldp_pvxs_driver_controller_channel_queue_depth",
                                                             "Number of items queued in each per-worker channel."))
    // Bus metrics
    , bus_push_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_push_total", "Number of events pushed onto the bus."))
    , bus_failure_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_failure_total", "Number of bus push failures reported by the MLDP gRPC API."))
    , bus_payload_bytes_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_payload_bytes_total", "Total protobuf payload bytes written to the MLDP ingestion stream."))
    , bus_payload_bytes_per_second_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_bus_payload_bytes_per_second", "Bytes/second for the most recent successful ingestion batch (protobuf payload bytes / end-to-end batch duration)."))
    , bus_stream_rotations_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_bus_stream_rotations_total", "Number of gRPC ingestion stream open/close cycles by reason."))
    // System CPU metrics
    , process_cpu_user_ticks_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_cpu_user_ticks_total", "Total user CPU time in clock ticks."))
    , process_cpu_system_ticks_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_cpu_system_ticks_total", "Total system CPU time in clock ticks."))
    , process_cpu_children_user_ticks_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_cpu_children_user_ticks_total", "Total children user CPU time in clock ticks."))
    , process_cpu_children_system_ticks_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_cpu_children_system_ticks_total", "Total children system CPU time in clock ticks."))
    // System memory metrics
    , process_memory_virtual_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_virtual_bytes", "Current virtual memory size in bytes."))
    , process_memory_rss_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_rss_bytes", "Current resident set size in bytes."))
    , process_memory_virtual_peak_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_virtual_peak_bytes", "Peak virtual memory size in bytes."))
    , process_memory_rss_anon_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_rss_anon_bytes", "Anonymous RSS in bytes."))
    , process_memory_rss_file_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_rss_file_bytes", "File-backed RSS in bytes."))
    , process_memory_rss_shmem_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_rss_shmem_bytes", "Shared memory RSS in bytes."))
    , process_memory_rss_total_bytes_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_memory_rss_total_bytes", "Total RSS (anon + file + shmem) in bytes."))
    // System I/O metrics
    , process_io_read_bytes_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_io_read_bytes_total", "Total bytes read from storage."))
    , process_io_write_bytes_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_io_write_bytes_total", "Total bytes written to storage."))
    , process_io_cancelled_write_bytes_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_io_cancelled_write_bytes_total", "Total bytes cancelled before write."))
    // System context switch metrics
    , process_context_switches_voluntary_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_context_switches_voluntary_total", "Total voluntary context switches."))
    , process_context_switches_involuntary_family_(makeCounterFamily(*registry_, "mldp_pvxs_driver_process_context_switches_involuntary_total", "Total involuntary context switches."))
    // System file descriptor metrics
    , process_fds_open_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_fds_open", "Number of open file descriptors."))
    // System thread metrics
    , process_threads_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_threads", "Number of threads."))
    // Process info metrics
    , process_priority_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_priority", "Process priority."))
    , process_nice_family_(makeGaugeFamily(*registry_, "mldp_pvxs_driver_process_nice", "Nice value."))
{
    if (config_.valid() && !config_.endpoint().empty())
    {
        exposer_ = std::make_unique<prometheus::Exposer>(config_.endpoint());
        exposer_->RegisterCollectable(registry_);
    }

    // Start system metrics collection
    startSystemMetricsCollection();
}

Metrics::~Metrics()
{
    stopSystemMetricsCollection();
}

void Metrics::startSystemMetricsCollection()
{
    // Create the metrics collector for the current process
    pid_t self_pid = getpid();
    system_metrics_collector_ = std::make_unique<metricsgrabber::MetricsCollector>(self_pid);

    // Start the collection thread
    stop_system_metrics_.store(false);
    system_metrics_thread_ = std::thread(&Metrics::collectSystemMetricsLoop, this);
}

void Metrics::stopSystemMetricsCollection()
{
    stop_system_metrics_.store(true);
    if (system_metrics_thread_.joinable())
    {
        system_metrics_thread_.join();
    }
}

void Metrics::collectSystemMetricsLoop()
{
    using namespace std::chrono_literals;

    while (!stop_system_metrics_.load())
    {
        if (system_metrics_collector_)
        {
            auto metricResult = system_metrics_collector_->collect();
            if (metricResult.has_value())
            {
                const auto&        snapshot = *metricResult;
                prometheus::Labels labels = {};

                // Update CPU metrics (counters)
                if (snapshot.utime)
                {
                    process_cpu_user_ticks_family_.Add(labels).Increment(static_cast<double>(*snapshot.utime));
                }
                if (snapshot.stime)
                {
                    process_cpu_system_ticks_family_.Add(labels).Increment(static_cast<double>(*snapshot.stime));
                }
                if (snapshot.cutime)
                {
                    process_cpu_children_user_ticks_family_.Add(labels).Increment(static_cast<double>(*snapshot.cutime));
                }
                if (snapshot.cstime)
                {
                    process_cpu_children_system_ticks_family_.Add(labels).Increment(static_cast<double>(*snapshot.cstime));
                }

                // Update memory metrics (gauges)
                if (snapshot.vm_size)
                {
                    process_memory_virtual_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.vm_size));
                }
                if (snapshot.vm_rss)
                {
                    process_memory_rss_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.vm_rss));
                }
                if (snapshot.vm_peak)
                {
                    process_memory_virtual_peak_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.vm_peak));
                }
                if (snapshot.rss_anon)
                {
                    process_memory_rss_anon_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.rss_anon));
                }
                if (snapshot.rss_file)
                {
                    process_memory_rss_file_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.rss_file));
                }
                if (snapshot.rss_shmem)
                {
                    process_memory_rss_shmem_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.rss_shmem));
                }
                if (snapshot.total_rss())
                {
                    process_memory_rss_total_bytes_family_.Add(labels).Set(static_cast<double>(*snapshot.total_rss()));
                }

                // Update I/O metrics (counters)
                if (snapshot.read_bytes)
                {
                    process_io_read_bytes_family_.Add(labels).Increment(static_cast<double>(*snapshot.read_bytes));
                }
                if (snapshot.write_bytes)
                {
                    process_io_write_bytes_family_.Add(labels).Increment(static_cast<double>(*snapshot.write_bytes));
                }
                if (snapshot.cancelled_write_bytes)
                {
                    process_io_cancelled_write_bytes_family_.Add(labels).Increment(static_cast<double>(*snapshot.cancelled_write_bytes));
                }

                // Update context switch metrics (counters)
                if (snapshot.voluntary_ctxt_switches)
                {
                    process_context_switches_voluntary_family_.Add(labels).Increment(static_cast<double>(*snapshot.voluntary_ctxt_switches));
                }
                if (snapshot.nonvoluntary_ctxt_switches)
                {
                    process_context_switches_involuntary_family_.Add(labels).Increment(static_cast<double>(*snapshot.nonvoluntary_ctxt_switches));
                }

                // Update file descriptor metrics (gauge)
                if (snapshot.num_fds)
                {
                    process_fds_open_family_.Add(labels).Set(static_cast<double>(*snapshot.num_fds));
                }

                // Update thread count (gauge)
                if (snapshot.num_threads)
                {
                    process_threads_family_.Add(labels).Set(static_cast<double>(*snapshot.num_threads));
                }

                // Update process priority info (gauges)
                if (snapshot.priority)
                {
                    process_priority_family_.Add(labels).Set(static_cast<double>(*snapshot.priority));
                }
                if (snapshot.nice)
                {
                    process_nice_family_.Add(labels).Set(static_cast<double>(*snapshot.nice));
                }
            }
            else if (metricResult.has_error())
            {
                // Log the error but continue - metrics collection is best effort and shouldn't crash the driver
                errorf("Failed to collect system metrics: {}", metricResult.error());
            }
        }

        // Sleep for configured scan interval
        std::this_thread::sleep_for(std::chrono::seconds(config_.scanIntervalSeconds()));
    }
}

std::shared_ptr<prometheus::Registry> Metrics::registry() const
{
    return registry_;
}

void Metrics::incrementReaderEvents(double value, prometheus::Labels tags)
{

    reader_events_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementReaderEventsReceived(double value, prometheus::Labels tags)
{
    reader_events_received_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementReaderErrors(double value, prometheus::Labels tags)
{

    reader_errors_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::observeReaderProcessingTimeMs(double value, prometheus::Labels tags)
{
    reader_processing_time_ms_family_.Add(std::move(tags), reader_processing_time_ms_buckets_).Observe(value);
}

void Metrics::setReaderQueueDepth(double value, prometheus::Labels tags)
{
    reader_queue_depth_family_.Add(std::move(tags)).Set(value);
}

void Metrics::setReaderPoolQueueDepth(double value, prometheus::Labels tags)
{
    reader_pool_queue_depth_family_.Add(std::move(tags)).Set(value);
}

double Metrics::readerEventsReceivedTotal() const
{
    return reader_events_received_family_.Add({}).Value();
}

void Metrics::setPoolConnectionsInUse(double value, prometheus::Labels tags)
{
    pool_connections_in_use_family_.Add(std::move(tags)).Set(value);
}

void Metrics::setPoolConnectionsAvailable(double value, prometheus::Labels tags)
{
    pool_connections_available_family_.Add(std::move(tags)).Set(value);
}

double Metrics::poolConnectionsInUse(prometheus::Labels tags) const
{
    return pool_connections_in_use_family_.Add(std::move(tags)).Value();
}

double Metrics::poolConnectionsAvailable(prometheus::Labels tags) const
{
    return pool_connections_available_family_.Add(std::move(tags)).Value();
}

void Metrics::observeControllerSendTimeSeconds(double value, prometheus::Labels tags)
{
    controller_send_time_family_.Add(std::move(tags), controller_send_time_buckets_).Observe(value);
}

void Metrics::setControllerQueueDepth(double value, prometheus::Labels tags)
{
    controller_queue_depth_family_.Add(std::move(tags)).Set(value);
}

void Metrics::setControllerChannelQueueDepth(double value, prometheus::Labels tags)
{
    controller_channel_queue_depth_family_.Add(std::move(tags)).Set(value);
}

void Metrics::incrementBusPushes(double value, prometheus::Labels tags)
{
    bus_push_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementBusFailures(double value, prometheus::Labels tags)
{
    bus_failure_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::incrementBusPayloadBytes(double value, prometheus::Labels tags)
{
    bus_payload_bytes_family_.Add(std::move(tags)).Increment(value);
}

void Metrics::setBusPayloadBytesPerSecond(double value, prometheus::Labels tags)
{
    bus_payload_bytes_per_second_family_.Add(std::move(tags)).Set(value);
}

void Metrics::incrementBusStreamRotations(double value, prometheus::Labels tags)
{
    bus_stream_rotations_family_.Add(std::move(tags)).Increment(value);
}

double Metrics::busPushTotal(prometheus::Labels tags) const
{
    return bus_push_family_.Add(std::move(tags)).Value();
}

double Metrics::busFailuresTotal(prometheus::Labels tags) const
{
    return bus_failure_family_.Add(std::move(tags)).Value();
}

double Metrics::busPayloadBytesTotal(prometheus::Labels tags) const
{
    return bus_payload_bytes_family_.Add(std::move(tags)).Value();
}

double Metrics::busPayloadBytesPerSecond(prometheus::Labels tags) const
{
    return bus_payload_bytes_per_second_family_.Add(std::move(tags)).Value();
}
