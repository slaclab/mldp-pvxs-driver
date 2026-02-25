//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <metrics/MetricsSnapshot.h>
#include <metrics/Metrics.h>

#include <prometheus/text_serializer.h>

#include <iostream>
#include <iomanip>
#include <sstream>
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
    std::map<std::string, std::map<std::string, double>> reader_metrics; // reader -> metric_type -> value
    double pool_in_use = 0.0;
    double pool_available = 0.0;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        // Skip comments and empty lines
        if (line.empty() || line.front() == '#')
            continue;

        // Parse bus push metrics (per reader)
        if (line.find("mldp_pvxs_driver_bus_push_total") != std::string::npos)
        {
            auto reader = extractLabelValue(line, "reader=");
            if (reader.empty())
            {
                reader = extractLabelValue(line, "source=");
            }
            if (!reader.empty())
            {
                reader_metrics[reader]["pushes"] = extractMetricValue(line);
            }
        }
        // Parse payload bytes total (per reader)
        else if (line.find("mldp_pvxs_driver_bus_payload_bytes_total") != std::string::npos)
        {
            auto reader = extractLabelValue(line, "reader=");
            if (reader.empty())
            {
                reader = extractLabelValue(line, "source=");
            }
            if (!reader.empty())
            {
                reader_metrics[reader]["bytes_total"] = extractMetricValue(line);
            }
        }
        // Parse payload bytes per second (per reader)
        else if (line.find("mldp_pvxs_driver_bus_payload_bytes_per_second") != std::string::npos)
        {
            auto reader = extractLabelValue(line, "reader=");
            if (reader.empty())
            {
                reader = extractLabelValue(line, "source=");
            }
            if (!reader.empty())
            {
                reader_metrics[reader]["bytes_per_sec"] = extractMetricValue(line);
            }
        }
        // Parse pool metrics
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
    
    const auto getMetric = [](const std::map<std::string, double>& metrics_data, std::string_view key)
    {
        const auto it = metrics_data.find(std::string(key));
        if (it == metrics_data.end())
        {
            return 0.0;
        }
        return it->second;
    };

    // Convert reader metrics to structured format
    for (const auto& [reader, metrics_data] : reader_metrics)
    {
        ReaderMetrics rm;
        rm.pv_name = reader;
        rm.pushes = static_cast<long long>(getMetric(metrics_data, "pushes"));
        rm.bytes_total = getMetric(metrics_data, "bytes_total");
        rm.bytes_per_sec = getMetric(metrics_data, "bytes_per_sec");
        snapshot.readers.push_back(rm);
    }
    
    // Set pool metrics
    snapshot.pool.in_use = static_cast<long long>(pool_in_use);
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
