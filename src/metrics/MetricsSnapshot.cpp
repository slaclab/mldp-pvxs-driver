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

#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace mldp_pvxs_driver::metrics {

std::string MetricsSnapshot::formatBytes(double bytes)
{
    // Format bytes in human-readable units
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

    // Parse prometheus text to extract metrics
    std::map<std::string, std::map<std::string, double>> reader_metrics; // source  -> metric -> value
    std::map<std::string, std::map<std::string, double>> writer_metrics; // writer  -> metric -> value
    double                                               pool_in_use    = 0.0;
    double                                               pool_available = 0.0;

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
            // labeled by both source= (reader) and writer= (writer instance)
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
            // send_time has no writer= label — accumulate globally
            writer_metrics["__global__"]["send_sum"] += extractMetricValue(line);
        }
        else if (line.find("mldp_pvxs_driver_controller_send_time_seconds_count") != std::string::npos)
        {
            // send_time has no writer= label — accumulate globally
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
    }

    // Build snapshot
    MetricsData snapshot;

    const auto getMetric = [](const std::map<std::string, double>& m, std::string_view key) -> double
    {
        const auto it = m.find(std::string(key));
        return (it != m.end()) ? it->second : 0.0;
    };

    for (const auto& [source, m] : reader_metrics)
    {
        ReaderMetrics rm;
        rm.pv_name      = source;
        rm.pushes       = static_cast<long long>(getMetric(m, "pushes"));
        rm.bytes_total  = getMetric(m, "bytes_total");
        rm.bytes_per_sec = getMetric(m, "bytes_per_sec");
        snapshot.readers.push_back(rm);
    }

    // Global metrics (no writer= label) shared across all writer instances
    const auto& global_m      = writer_metrics.count("__global__") ? writer_metrics.at("__global__")
                                                                    : std::map<std::string, double>{};
    const double global_data_bps   = getMetric(global_m, "data_bps");
    const double global_send_sum   = getMetric(global_m, "send_sum");
    const double global_send_count = getMetric(global_m, "send_count");

    for (const auto& [writer, m] : writer_metrics)
    {
        if (writer == "__global__")
            continue;
        WriterMetrics wm;
        wm.writer_name           = writer;
        wm.queue_depth           = static_cast<long long>(getMetric(m, "queue_depth"));
        wm.stream_rotations      = static_cast<long long>(getMetric(m, "stream_rotations"));
        wm.failures              = static_cast<long long>(getMetric(m, "failures"));
        wm.payload_bytes_per_sec = getMetric(m, "payload_bps");
        wm.data_bytes_per_sec    = global_data_bps;
        wm.send_time_mean_ms     = (global_send_count > 0.0)
                                       ? (global_send_sum / global_send_count * 1000.0)
                                       : 0.0;
        snapshot.writers.push_back(wm);
    }

    snapshot.pool.in_use    = static_cast<long long>(pool_in_use);
    snapshot.pool.available = static_cast<long long>(pool_available);

    return snapshot;
}

std::string MetricsSnapshot::toString(const MetricsData& snapshot)
{
    std::ostringstream output;
    output << "================================ METRICS DUMP ========================\n\n";

    // Print per-reader metrics
    if (!snapshot.readers.empty())
    {
        output << "READER STATISTICS:\n";
        output << "─────────────────────────────────────────────────────────────────\n";
        for (const auto& reader : snapshot.readers)
        {
            output << "PV: " << reader.pv_name << "\n";
            output << "  Pushes:     " << reader.pushes << "\n";
            output << "  Total Data: " << formatBytes(reader.bytes_total) << "\n";
            output << "  Rate:       " << formatBytes(reader.bytes_per_sec) << "/s\n";
            output << "\n";
        }
    }

    // Print per-writer performance metrics
    if (!snapshot.writers.empty())
    {
        output << "WRITER PERFORMANCE:\n";
        output << "─────────────────────────────────────────────────────────────────\n";
        for (const auto& w : snapshot.writers)
        {
            output << "Writer: " << w.writer_name << "\n";
            output << "  Queue Depth:      " << w.queue_depth << "\n";
            output << "  Stream Rotations: " << w.stream_rotations << "\n";
            output << "  Failures:         " << w.failures << "\n";
            output << "  Payload Rate:     " << formatBytes(w.payload_bytes_per_sec) << "/s\n";
            output << "  Data Rate:        " << formatBytes(w.data_bytes_per_sec)    << "/s\n";
            output << "  Send Latency:     " << std::fixed << std::setprecision(3)
                   << w.send_time_mean_ms << " ms (mean)\n";
            output << "\n";
        }
    }

    // Print pool statistics
    output << "CONNECTION POOL:\n";
    output << "─────────────────────────────────────────────────────────────────\n";
    output << "  In Use:     " << snapshot.pool.in_use << "\n";
    output << "  Available:  " << snapshot.pool.available << "\n";
    output << "  Total:      " << snapshot.pool.total() << "\n";

    output << "=====================================================================\n";

    return output.str();
}

} // namespace mldp_pvxs_driver::metrics
