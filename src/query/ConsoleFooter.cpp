//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/ConsoleFooter.h>

#include <chrono>
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

} // namespace

std::string FooterRenderer::render(const ConsoleStatus& status, const int terminal_width) const
{
    if (terminal_width <= 0) return {};
    std::string line;
    if (status.query_running)
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
