//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <PeriodicMetricsDumper.h>

#include <prometheus/text_serializer.h>
#include <spdlog/spdlog.h>
#include <util/StringFormat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>

using namespace mldp_pvxs_driver::metrics;

namespace {

bool endsWith(std::string_view value, std::string_view suffix)
{
    if (suffix.size() > value.size())
        return false;
    return value.substr(value.size() - suffix.size()) == suffix;
}

std::string escapeJsonString(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const unsigned char ch : input)
    {
        switch (ch)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04X", ch);
                out += buf;
            }
            else
            {
                out += static_cast<char>(ch);
            }
            break;
        }
    }
    return out;
}

} // namespace

// Background thread to periodically dump metrics to a file.
PeriodicMetricsDumper::PeriodicMetricsDumper(mldp_pvxs_driver::metrics::Metrics& metrics, const std::string& path, std::chrono::milliseconds dump_interval)
    : metrics(metrics), output_path(path), interval(dump_interval)
{
    start();
}

PeriodicMetricsDumper::~PeriodicMetricsDumper()
{
    if (!should_stop)
    {
        stop();
    }
}

void PeriodicMetricsDumper::start()
{
    should_stop = false;
    dump_thread = std::thread([this]()
                              {
                                  while (!should_stop)
                                  {
                                      {
                                          std::unique_lock<std::mutex> lock(state_mutex);
                                          // Wait for either the interval to elapse or stop signal
                                          stop_signal.wait_for(lock, interval, [this]()
                                                               {
                                                                   return should_stop.load();
                                                               });
                                      }

                                      if (should_stop)
                                          break;

                                      std::lock_guard<std::mutex> lock(output_path_mutex);
                                      if (!output_path.empty())
                                      {
                                          spdlog::debug("Dumping metrics to file '{}'", output_path);
                                          appendMetricsToFile();
                                      }
                                  }
                              });
}

void PeriodicMetricsDumper::stop()
{
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        should_stop = true;
    }
    // Wake up the thread immediately so it doesn't wait for the full interval
    stop_signal.notify_one();

    if (dump_thread.joinable())
    {
        dump_thread.join();
    }
}

// Serialize metrics to JSON Lines format (one JSON object per line with timestamp).
std::string PeriodicMetricsDumper::serializeMetricsJsonl()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());

    std::ostringstream out;
    out << "{\n";
    out << "  \"timestamp_ms\": " << ms.count() << ",\n";
    out << "  \"timestamp_iso\": \"";

    // Format ISO 8601 timestamp
    auto       time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm* tm_info = std::gmtime(&time_t_now);
    char       iso_buf[30];
    std::strftime(iso_buf, sizeof(iso_buf), "%Y-%m-%dT%H:%M:%S", tm_info);
    out << iso_buf << "Z\",\n";

    // Serialize prometheus metrics as text and parse into JSON
    prometheus::TextSerializer serializer;
    std::ostringstream         metrics_out;
    serializer.Serialize(metrics_out, metrics.registry()->Collect());

    // Extract metrics from prometheus format
    std::string      metrics_text = metrics_out.str();
    std::string_view remaining = metrics_text;

    // First pass: collect all metrics grouped by name
    std::map<std::string, std::vector<std::string>> metrics_by_name;

    while (!remaining.empty())
    {
        const auto             newline = remaining.find('\n');
        const std::string_view line = (newline == std::string_view::npos) ? remaining
                                                                          : remaining.substr(0, newline);

        // Skip comment lines and empty lines
        if (!line.empty() && line.front() != '#' && line.back() != '\0')
        {
            // Parse prometheus line format: metric_name{labels} value timestamp
            const auto space1 = line.find(' ');
            if (space1 != std::string_view::npos)
            {
                const auto space2 = line.find(' ', space1 + 1);
                const auto brace_open = line.find('{');
                const auto brace_close = line.find('}');

                std::string_view metric_name;
                std::string_view labels_str;
                std::string_view value;

                // Extract metric name and labels
                if (brace_open != std::string_view::npos && brace_close != std::string_view::npos)
                {
                    // Metric has labels: metric_name{label1="value1",label2="value2"} value
                    metric_name = line.substr(0, brace_open);
                    labels_str = line.substr(brace_open + 1, brace_close - brace_open - 1);
                }
                else
                {
                    // Metric has no labels: metric_name value
                    metric_name = line.substr(0, space1);
                    labels_str = "";
                }

                value = (space2 != std::string_view::npos)
                            ? line.substr(space1 + 1, space2 - space1 - 1)
                            : line.substr(space1 + 1);

                // Build metric entry JSON
                std::ostringstream metric_entry;
                metric_entry << "{";

                std::string metric_name_str(metric_name);
                std::string histogram_kind;
                if (endsWith(metric_name, "_bucket"))
                {
                    metric_name_str = std::string(metric_name.substr(0, metric_name.size() - 7));
                    histogram_kind = "bucket";
                }
                else if (endsWith(metric_name, "_sum"))
                {
                    metric_name_str = std::string(metric_name.substr(0, metric_name.size() - 4));
                    histogram_kind = "sum";
                }
                else if (endsWith(metric_name, "_count"))
                {
                    metric_name_str = std::string(metric_name.substr(0, metric_name.size() - 6));
                    histogram_kind = "count";
                }

                // Parse and output labels as structured JSON
                if (!labels_str.empty())
                {
                    bool first_label = true;

                    // Parse labels in format: label1="value1",label2="value2"
                    std::string_view remaining_labels = labels_str;
                    while (!remaining_labels.empty())
                    {
                        // Find the next '=' to get label name
                        const auto eq_pos = remaining_labels.find('=');
                        if (eq_pos == std::string_view::npos)
                            break;

                        const std::string_view label_name = remaining_labels.substr(0, eq_pos);
                        remaining_labels.remove_prefix(eq_pos + 1);

                        // Find the quoted value
                        if (remaining_labels.empty() || remaining_labels[0] != '"')
                            break;

                        remaining_labels.remove_prefix(1); // Skip opening quote
                        std::string label_value;
                        bool        escaped = false;

                        // Extract quoted value, handling escapes
                        for (size_t i = 0; i < remaining_labels.size(); ++i)
                        {
                            const char ch = remaining_labels[i];
                            if (escaped)
                            {
                                label_value += ch;
                                escaped = false;
                            }
                            else if (ch == '\\')
                            {
                                label_value += ch;
                                escaped = true;
                            }
                            else if (ch == '"')
                            {
                                remaining_labels.remove_prefix(i + 1);
                                break;
                            }
                            else
                            {
                                label_value += ch;
                            }
                        }

                        // Output label (rename "reader" to "source")
                        if (!first_label)
                            metric_entry << ", ";

                        std::string final_label_name(label_name);
                        if (final_label_name == "reader")
                        {
                            final_label_name = "source";
                        }

                        metric_entry << "\"" << escapeJsonString(final_label_name) << "\": \"" << escapeJsonString(label_value)
                                     << "\"";

                        first_label = false;

                        // Skip comma if present
                        if (!remaining_labels.empty() && remaining_labels[0] == ',')
                        {
                            remaining_labels.remove_prefix(1);
                        }
                    }

                    if (!histogram_kind.empty())
                    {
                        metric_entry << ", \"histogram\": \"" << histogram_kind << "\"";
                    }
                    metric_entry << ", \"value\": " << value << "}";
                }
                else
                {
                    // Metric with no labels - just output value object
                    if (!histogram_kind.empty())
                    {
                        metric_entry << "\"histogram\": \"" << histogram_kind << "\", ";
                    }
                    metric_entry << "\"value\": " << value << "}";
                }

                // Add to grouped metrics
                metrics_by_name[metric_name_str].push_back(metric_entry.str());
            }
        }

        if (newline == std::string_view::npos)
            break;
        remaining.remove_prefix(newline + 1);
    }

    // Second pass: output grouped metrics as arrays
    out << "  \"metrics\": {\n";
    bool first_metric = true;

    for (const auto& [metric_name, entries] : metrics_by_name)
    {
        if (!first_metric)
            out << ",\n";

        out << "    \"" << escapeJsonString(metric_name) << "\": [";

        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i > 0)
                out << ", ";
            out << entries[i];
        }

        out << "]";
        first_metric = false;
    }

    out << "\n  }\n}\n";
    return out.str();
}

void PeriodicMetricsDumper::appendMetricsToFile()
{
    try
    {
        const auto    jsonl = serializeMetricsJsonl();
        std::ofstream ofs(output_path, std::ios::out | std::ios::app); // Append mode
        if (!ofs)
        {
            throw std::runtime_error(mldp_pvxs_driver::util::format_string("Failed to open metrics output file '{}'", output_path));
        }
        ofs << jsonl;
        ofs.close();
    }
    catch (const std::exception& e)
    {
        spdlog::error("Failed to append metrics to file: {}", e.what());
    }
}
