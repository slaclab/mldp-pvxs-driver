//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/ConsoleFooter.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include <sys/ioctl.h>
#include <unistd.h>

using namespace mldp_pvxs_driver::cli;

namespace {

void appendField(std::string& line, const std::string_view field, const int width)
{
    if (line.empty())
    {
        line.assign(field.substr(0, static_cast<std::size_t>(width)));
        return;
    }
    if (static_cast<int>(line.size() + 3 + field.size()) <= width)
    {
        line += " | ";
        line += field;
    }
}

std::string elapsed(const std::chrono::milliseconds value)
{
    const auto seconds = value.count() / 1000;
    return std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s";
}

std::string formatBytes(const uint64_t bytes)
{
    constexpr std::array<std::string_view, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
    double                                      value = static_cast<double>(bytes);
    std::size_t                                 unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size())
    {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream output;
    if (unit == 0 || value >= 10.0)
    {
        output << std::fixed << std::setprecision(0);
    }
    else
    {
        output << std::fixed << std::setprecision(1);
    }
    output << value << ' ' << units[unit];
    return output.str();
}

} // namespace

std::string FooterRenderer::render(const ConsoleStatus& status, const int terminal_width) const
{
    if (terminal_width <= 0) return {};
    std::string line;
    if (!status.error.empty())
    {
        appendField(line, "Error: " + status.error, terminal_width);
    }
    else if (status.cancelled)
    {
        appendField(line, "Query cancelled", terminal_width);
    }
    else if (status.query_running)
    {
        if (status.progress)
        {
            const auto& progress = *status.progress;
            appendField(line, "Running: " + std::string(query::queryProgressPhaseName(progress.phase)), terminal_width);
            appendField(line, elapsed(progress.elapsed), terminal_width);
            if (!progress.table_name.empty()) appendField(line, progress.table_name, terminal_width);
            if (!progress.operation.empty()) appendField(line, progress.operation, terminal_width);
            if (!progress.detail.empty()) appendField(line, progress.detail, terminal_width);
            if (progress.result_page > 0) appendField(line, "result page " + std::to_string(progress.result_page), terminal_width);
            if (progress.window_index > 0)
            {
                std::string shard = "window " + std::to_string(progress.window_index);
                if (progress.slice_index > 0) shard += ", slice " + std::to_string(progress.slice_index);
                if (progress.series_shard_index > 0) shard += ", series shard " + std::to_string(progress.series_shard_index);
                appendField(line, shard, terminal_width);
            }
            if (progress.rpc_calls_started > 0)
                appendField(line, std::to_string(progress.rpc_calls_completed) + "/" + std::to_string(progress.rpc_calls_started) + " RPCs", terminal_width);
            if (progress.rows_from_backend > 0) appendField(line, std::to_string(progress.rows_from_backend) + " backend rows", terminal_width);
        }
        else appendField(line, "Running", terminal_width);
        appendField(line, "Ctrl-C cancel", terminal_width);
    }
    else
    {
        appendField(line, "Query: ready", terminal_width);
    }
    if (status.completed_stats)
    {
        const auto& stats = *status.completed_stats;
        const auto  filtered = stats.rows_from_backend >= stats.rows_returned
                                   ? stats.rows_from_backend - stats.rows_returned
                                   : 0;
        appendField(line, "Query completed", terminal_width);
        appendField(line,
                    std::to_string(stats.rows_returned) + "/" + std::to_string(stats.rows_from_backend) + " rows (" +
                        std::to_string(filtered) + " filtered)",
                    terminal_width);
        appendField(line, std::to_string(stats.elapsed.count()) + " ms", terminal_width);
        appendField(line, std::to_string(stats.rpc_calls) + " RPC", terminal_width);
        appendField(line, formatBytes(stats.bytes_spilled) + " spilled", terminal_width);
        appendField(line,
                    formatBytes(stats.materialized_bytes) + " materialized / " + std::to_string(stats.materialized_files) + " files",
                    terminal_width);
        appendField(line, formatBytes(stats.peak_memory_bytes) + " peak", terminal_width);
    }
    line.resize(static_cast<std::size_t>(terminal_width), ' ');
    return line;
}

TerminalLayout::TerminalLayout(std::ostream& output, std::shared_ptr<std::mutex> output_mutex)
    : output_(output)
    , output_mutex_(std::move(output_mutex))
{
}

TerminalLayout::~TerminalLayout()
{
    restore();
}

bool TerminalLayout::initialize()
{
    if (!updateSize()) return false;
    initialized_ = true;
    configureScrollRegion();
    output_.flush();
    return true;
}

void TerminalLayout::redraw(const ConsoleStatus& status)
{
    if (!initialized_) return;
    if (!updateSize())
    {
        restore();
        return;
    }
    configureScrollRegion();
    draw(status);
}

void TerminalLayout::positionInputCursor()
{
    if (!initialized_) return;
    output_ << "\x1b[" << (rows_ - 1) << ";1H";
    output_.flush();
}

void TerminalLayout::restore() noexcept
{
    if (!initialized_) return;
    try
    {
        output_ << "\x1b[r\x1b[0m";
        if (updateSize()) output_ << "\x1b[" << rows_ << ";1H\x1b[2K";
        output_.flush();
    }
    catch (...)
    {
    }
    initialized_ = false;
}

bool TerminalLayout::active() const noexcept
{
    return initialized_;
}

bool TerminalLayout::updateSize()
{
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row < 2 || size.ws_col == 0) return false;
    rows_ = static_cast<int>(size.ws_row);
    columns_ = static_cast<int>(size.ws_col);
    return true;
}

void TerminalLayout::configureScrollRegion()
{
    output_ << "\x1b[1;" << (rows_ - 1) << "r";
}

void TerminalLayout::draw(const ConsoleStatus& status)
{
    static std::mutex fallback_output_mutex;
    std::unique_lock lock(output_mutex_ ? *output_mutex_ : fallback_output_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    output_ << "\x1b[s"
            << "\x1b[" << rows_ << ";1H"
            << "\x1b[2K"
            << "\x1b[7m"
            << footer_renderer_.render(status, columns_)
            << "\x1b[0m"
            << "\x1b[u";
    output_.flush();
}
