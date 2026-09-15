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
#include <metrics/MetricsSnapshot.h>

#include <prometheus/text_serializer.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace mldp_pvxs_driver::metrics {

std::string MetricsSnapshot::formatBytes(double bytes)
{
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;

    if (bytes >= GB)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (bytes / GB) << " GB";
        return oss.str();
    }
    if (bytes >= MB)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (bytes / MB) << " MB";
        return oss.str();
    }
    if (bytes >= KB)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (bytes / KB) << " KB";
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << bytes << " B";
    return oss.str();
}

std::string MetricsSnapshot::extractLabelValue(std::string_view line, std::string_view label)
{
    const auto label_start = line.find(label);
    if (label_start == std::string_view::npos)
    {
        return "";
    }
    const auto quote_start = line.find('"', label_start);
    if (quote_start == std::string_view::npos)
    {
        return "";
    }
    const auto quote_end = line.find('"', quote_start + 1);
    if (quote_end == std::string_view::npos)
    {
        return "";
    }
    return std::string(line.substr(quote_start + 1, quote_end - quote_start - 1));
}

double MetricsSnapshot::extractMetricValue(std::string_view line)
{
    const auto last_space = line.rfind(' ');
    if (last_space == std::string_view::npos)
    {
        return 0.0;
    }
    try
    {
        return std::stod(std::string(line.substr(last_space + 1)));
    }
    catch (...)
    {
        return 0.0;
    }
}

std::string MetricsSnapshot::serializeMetricsText(const Metrics& metrics)
{
    prometheus::TextSerializer serializer;
    std::ostringstream         out;
    serializer.Serialize(out, metrics.registry()->Collect());
    return out.str();
}

MetricsData MetricsSnapshot::getSnapshot(const Metrics& metrics) const
{
    const auto text = serializeMetricsText(metrics);

    std::map<std::string, std::map<std::string, double>> reader_metrics;
    std::map<std::string, std::map<std::string, double>> writer_metrics;
    double                                               pool_in_use    = 0.0;
    double                                               pool_available = 0.0;
    ProcessMetrics                                       proc;

    std::istringstream stream(text);
    std::string        line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
            continue;

        // --- reader/source-level metrics ---
        if (line.find("mldp_pvxs_driver_writer_push_total") != std::string::npos)
        {
            const auto source = extractLabelValue(line, "source=");
            if (!source.empty())
                reader_metrics[source]["pushes"] = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_writer_payload_bytes_total") != std::string::npos)
        {
            const auto source = extractLabelValue(line, "source=");
            if (!source.empty())
                reader_metrics[source]["bytes_total"] = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_writer_payload_bytes_per_second") != std::string::npos)
        {
            const auto source = extractLabelValue(line, "source=");
            if (!source.empty())
                reader_metrics[source]["bytes_per_sec"] = extractMetricValue(line);
            const auto writer = extractLabelValue(line, "writer=");
            if (!writer.empty())
                writer_metrics[writer]["payload_bps"] += extractMetricValue(line);
        }

        // --- writer-instance-level metrics ---
        else if (line.find("mldp_pvxs_driver_writer_queue_depth") != std::string::npos)
        {
            const auto writer = extractLabelValue(line, "writer=");
            if (!writer.empty())
                writer_metrics[writer]["queue_depth"] = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_writer_stream_rotations_total") != std::string::npos)
        {
            const auto writer = extractLabelValue(line, "writer=");
            if (!writer.empty())
                writer_metrics[writer]["stream_rotations"] += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_writer_failure_total") != std::string::npos)
        {
            const auto writer = extractLabelValue(line, "writer=");
            if (!writer.empty())
                writer_metrics[writer]["failures"] += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_writer_data_bytes_per_second") != std::string::npos)
        {
            const auto writer = extractLabelValue(line, "writer=");
            if (!writer.empty())
                writer_metrics[writer]["data_bps"] += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_controller_send_time_seconds_sum") != std::string::npos)
        {
            writer_metrics["__global__"]["send_sum"] += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_controller_send_time_seconds_count") != std::string::npos)
        {
            writer_metrics["__global__"]["send_count"] += extractMetricValue(line);
        }

        // --- pool metrics ---
        else if (line.find("mldp_pvxs_driver_pool_connections_in_use") != std::string::npos)
        {
            pool_in_use = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_pool_connections_available") != std::string::npos)
        {
            pool_available = extractMetricValue(line);
        }

        // --- process/CPU metrics ---
        else if (line.find("mldp_pvxs_driver_process_cpu_user_ticks_total") != std::string::npos)
        {
            proc.cpu_user_ticks += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_cpu_system_ticks_total") != std::string::npos)
        {
            proc.cpu_system_ticks += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_threads") != std::string::npos)
        {
            proc.threads = static_cast<long long>(extractMetricValue(line));
        }
        else if (line.find("mldp_pvxs_driver_process_fds_open") != std::string::npos)
        {
            proc.fds_open = static_cast<long long>(extractMetricValue(line));
        }
        else if (line.find("mldp_pvxs_driver_process_memory_virtual_peak_bytes") != std::string::npos)
        {
            proc.vm_peak_bytes = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_memory_virtual_bytes") != std::string::npos)
        {
            proc.vm_size_bytes = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_memory_rss_total_bytes") != std::string::npos)
        {
            proc.rss_total_bytes = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_memory_rss_bytes") != std::string::npos)
        {
            proc.vm_rss_bytes = extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_io_read_bytes_total") != std::string::npos)
        {
            proc.io_read_bytes += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_process_io_write_bytes_total") != std::string::npos)
        {
            proc.io_write_bytes += extractMetricValue(line);
        }
    }

    MetricsData snapshot;

    const auto getMetric = [](const std::map<std::string, double>& m, std::string_view key) -> double
    {
        const auto it = m.find(std::string(key));
        return (it != m.end()) ? it->second : 0.0;
    };

    for (const auto& [source, m] : reader_metrics)
    {
        ReaderMetrics rm;
        rm.pv_name       = source;
        rm.pushes        = static_cast<long long>(getMetric(m, "pushes"));
        rm.bytes_total   = getMetric(m, "bytes_total");
        rm.bytes_per_sec = getMetric(m, "bytes_per_sec");
        snapshot.readers.push_back(rm);
    }

    const auto& global_m      = writer_metrics.count("__global__") ? writer_metrics.at("__global__")
                                                                    : std::map<std::string, double>{};
    const double global_send_sum   = getMetric(global_m, "send_sum");
    const double global_send_count = getMetric(global_m, "send_count");

    for (const auto& [writer, m] : writer_metrics)
    {
        if (writer == "__global__")
            continue;
        WriterSnapshot wm;
        wm.writer_name           = writer;
        wm.queue_depth           = static_cast<long long>(getMetric(m, "queue_depth"));
        wm.stream_rotations      = static_cast<long long>(getMetric(m, "stream_rotations"));
        wm.failures              = static_cast<long long>(getMetric(m, "failures"));
        wm.payload_bytes_per_sec = getMetric(m, "payload_bps");
        wm.data_bytes_per_sec    = getMetric(m, "data_bps");
        wm.send_time_mean_ms     = (global_send_count > 0.0)
                                       ? (global_send_sum / global_send_count * 1000.0)
                                       : 0.0;
        snapshot.writers.push_back(wm);
    }

    snapshot.pool.in_use    = static_cast<long long>(pool_in_use);
    snapshot.pool.available = static_cast<long long>(pool_available);
    snapshot.process        = proc;

    return snapshot;
}

std::string MetricsSnapshot::toString(const MetricsData& snapshot)
{
    std::ostringstream output;
    output << "================================ METRICS DUMP ========================\n\n";

    // --- Readers table ---
    if (!snapshot.readers.empty())
    {
        // Compute column widths
        std::size_t pv_w = 7; // "PV Name"
        for (const auto& r : snapshot.readers)
            pv_w = std::max(pv_w, r.pv_name.size());
        pv_w += 2;

        const std::string sep(pv_w + 10 + 12 + 14 + 6, '-');
        output << "READER STATISTICS:\n";
        output << sep << "\n";
        output << std::left << std::setw(static_cast<int>(pv_w)) << "PV Name"
               << std::right << std::setw(10) << "Pushes"
               << std::setw(12) << "Total Data"
               << std::setw(14) << "Rate" << "\n";
        output << sep << "\n";
        for (const auto& r : snapshot.readers)
        {
            output << std::left  << std::setw(static_cast<int>(pv_w)) << r.pv_name
                   << std::right << std::setw(10) << r.pushes
                   << std::setw(12) << formatBytes(r.bytes_total)
                   << std::setw(13) << (formatBytes(r.bytes_per_sec) + "/s") << "\n";
        }
        output << "\n";
    }

    // --- Writers table ---
    if (!snapshot.writers.empty())
    {
        std::size_t name_w = 6; // "Writer"
        for (const auto& w : snapshot.writers)
            name_w = std::max(name_w, w.writer_name.size());
        name_w += 2;

        const std::string sep(name_w + 7 + 11 + 6 + 14 + 14 + 12, '-');
        output << "WRITER PERFORMANCE:\n";
        output << sep << "\n";
        output << std::left  << std::setw(static_cast<int>(name_w)) << "Writer"
               << std::right << std::setw(7)  << "Queue"
               << std::setw(11) << "Rotations"
               << std::setw(6)  << "Fail"
               << std::setw(14) << "Payload Rate"
               << std::setw(14) << "Data Rate"
               << std::setw(12) << "Latency" << "\n";
        output << sep << "\n";
        for (const auto& w : snapshot.writers)
        {
            std::ostringstream lat;
            lat << std::fixed << std::setprecision(3) << w.send_time_mean_ms << " ms";
            output << std::left  << std::setw(static_cast<int>(name_w)) << w.writer_name
                   << std::right << std::setw(7)  << w.queue_depth
                   << std::setw(11) << w.stream_rotations
                   << std::setw(6)  << w.failures
                   << std::setw(14) << (formatBytes(w.payload_bytes_per_sec) + "/s")
                   << std::setw(14) << (formatBytes(w.data_bytes_per_sec)    + "/s")
                   << std::setw(12) << lat.str() << "\n";
        }
        output << "\n";
    }

    // --- Connection pool ---
    output << "CONNECTION POOL:\n";
    output << "─────────────────────────────────────────\n";
    output << "  In Use:    " << snapshot.pool.in_use    << "\n";
    output << "  Available: " << snapshot.pool.available << "\n";
    output << "  Total:     " << snapshot.pool.total()   << "\n";
    output << "\n";

    // --- Process / CPU / memory ---
    const auto& p = snapshot.process;
    output << "PROCESS METRICS:\n";
    output << "─────────────────────────────────────────\n";
    output << "  CPU (user ticks):   " << std::fixed << std::setprecision(0) << p.cpu_user_ticks   << "\n";
    output << "  CPU (sys  ticks):   " << std::fixed << std::setprecision(0) << p.cpu_system_ticks << "\n";
    output << "  Threads:            " << p.threads  << "\n";
    output << "  Open FDs:           " << p.fds_open << "\n";
    output << "  VM Size:            " << formatBytes(p.vm_size_bytes) << "\n";
    output << "  VM Peak:            " << formatBytes(p.vm_peak_bytes) << "\n";
    output << "  RSS:                " << formatBytes(p.vm_rss_bytes)  << "\n";
    output << "  RSS Total:          " << formatBytes(p.rss_total_bytes) << "\n";
    output << "  I/O Read  (total):  " << formatBytes(p.io_read_bytes)  << "\n";
    output << "  I/O Write (total):  " << formatBytes(p.io_write_bytes) << "\n";

    output << "=====================================================================\n";

    return output.str();
}

} // namespace mldp_pvxs_driver::metrics
