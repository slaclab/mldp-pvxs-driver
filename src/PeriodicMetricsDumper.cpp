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

#include <chrono>
#include <cstdio>
#include <fstream>

using namespace mldp_pvxs_driver::metrics;

namespace {

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
    out << "  \"metrics\": {\n";
    std::string      metrics_text = metrics_out.str();
    std::string_view remaining = metrics_text;
    bool             first_metric = true;

    while (!remaining.empty())
    {
        const auto             newline = remaining.find('\n');
        const std::string_view line = (newline == std::string_view::npos) ? remaining
                                                                          : remaining.substr(0, newline);

        // Skip comment lines and empty lines
        if (!line.empty() && line.front() != '#' && line.back() != '\0')
        {
            if (!first_metric)
                out << ",\n";

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

                // Output metric name
                out << "    \"" << escapeJsonString(metric_name) << "\": ";

                // Parse and output labels as structured JSON
                if (!labels_str.empty())
                {
                    out << "{";
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
                            out << ", ";

                        std::string final_label_name(label_name);
                        if (final_label_name == "reader")
                        {
                            final_label_name = "source";
                        }

                        out << "\"" << escapeJsonString(final_label_name) << "\": \"" << escapeJsonString(label_value)
                            << "\"";

                        first_label = false;

                        // Skip comma if present
                        if (!remaining_labels.empty() && remaining_labels[0] == ',')
                        {
                            remaining_labels.remove_prefix(1);
                        }
                    }

                    out << ", \"value\": " << value << "}";
                }
                else
                {
                    // Metric with no labels - just output value object
                    out << "{\"value\": " << value << "}";
                }

                first_metric = false;
            }
        }

        if (newline == std::string_view::npos)
            break;
        remaining.remove_prefix(newline + 1);
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
            throw std::runtime_error(std::format("Failed to open metrics output file '{}'", output_path));
        }
        ofs << jsonl;
        ofs.close();
    }
    catch (const std::exception& e)
    {
        spdlog::error("Failed to append metrics to file: {}", e.what());
    }
}
