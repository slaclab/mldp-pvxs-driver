#pragma once

#include <metrics/Metrics.h>
#include <metrics/MetricsSnapshot.h>

#include <prometheus/text_serializer.h>

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::testutil {

inline std::optional<mldp_pvxs_driver::metrics::ReaderMetrics> findReaderMetrics(
    const mldp_pvxs_driver::metrics::MetricsData& snapshot,
    const std::string&                            pvName)
{
    for (const auto& reader : snapshot.readers)
    {
        if (reader.pv_name == pvName)
        {
            return reader;
        }
    }
    return std::nullopt;
}

inline mldp_pvxs_driver::metrics::ReaderMetrics aggregateReaderMetrics(
    const mldp_pvxs_driver::metrics::MetricsData& snapshot)
{
    mldp_pvxs_driver::metrics::ReaderMetrics aggregated;
    for (const auto& reader : snapshot.readers)
    {
        aggregated.pushes += reader.pushes;
        aggregated.bytes_total += reader.bytes_total;
        aggregated.bytes_per_sec += reader.bytes_per_sec;
    }
    return aggregated;
}

inline std::string serializeMetricsText(const mldp_pvxs_driver::metrics::Metrics& metrics)
{
    prometheus::TextSerializer serializer;
    std::ostringstream         out;
    serializer.Serialize(out, metrics.registry()->Collect());
    return out.str();
}

inline std::string extractLabelValue(std::string_view line, std::string_view label)
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

inline double extractMetricValue(std::string_view line)
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

inline double getMetricValueForSource(const std::string& text, std::string_view metric, const std::string& source)
{
    std::istringstream stream(text);
    std::string        line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        if (line.find(metric) == std::string::npos)
        {
            continue;
        }
        const auto label_value = extractLabelValue(line, "source=");
        if (label_value == source)
        {
            return extractMetricValue(line);
        }
    }
    return 0.0;
}

inline double getGaugeValue(const std::string& text, std::string_view metric)
{
    std::istringstream stream(text);
    std::string        line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        if (line.find(metric) != std::string::npos)
        {
            return extractMetricValue(line);
        }
    }
    return 0.0;
}

} // namespace mldp_pvxs_driver::testutil
