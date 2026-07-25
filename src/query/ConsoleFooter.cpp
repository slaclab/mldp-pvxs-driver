//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/ConsoleFooter.h>

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

std::string bytes(const uint64_t value)
{
    constexpr uint64_t kMiB = 1024ULL * 1024ULL;
    if (value >= kMiB)
    {
        return std::to_string(value / kMiB) + " MiB";
    }
    if (value < 1024ULL)
    {
        return std::to_string(value) + " B";
    }
    return std::to_string(value / 1024ULL) + " KiB";
}

} // namespace

std::string FooterRenderer::render(const ConsoleStatus& status, const int terminal_width) const
{
    if (terminal_width <= 0)
    {
        return {};
    }

    std::string line;
    if (!status.error.empty())
    {
        appendField(line, "Error: " + status.error, terminal_width);
    }
    else if (status.query_running)
    {
        std::string phase = "Running";
        if (status.progress)
        {
            phase += ": ";
            phase += query::queryProgressPhaseName(status.progress->phase);
        }
        appendField(line, phase, terminal_width);
    }
    else
    {
        appendField(line, "Query: ready", terminal_width);
    }

    if (status.completed_stats)
    {
        const auto& stats = *status.completed_stats;
        appendField(line, std::to_string(stats.rows_returned) + " rows", terminal_width);
        appendField(line, std::to_string(stats.elapsed.count()) + " ms", terminal_width);
        appendField(line, std::to_string(stats.rpc_calls) + " RPCs", terminal_width);
        if (stats.bytes_spilled > 0)
        {
            appendField(line, "spill " + bytes(stats.bytes_spilled), terminal_width);
        }
        if (stats.peak_memory_bytes > 0)
        {
            appendField(line, "peak " + bytes(stats.peak_memory_bytes), terminal_width);
        }
    }
    line.resize(static_cast<std::size_t>(terminal_width), ' ');
    return line;
}

TerminalLayout::TerminalLayout(std::ostream& output)
    : output_(output)
{
}

TerminalLayout::~TerminalLayout()
{
    restore();
}

bool TerminalLayout::initialize()
{
    if (!updateSize())
    {
        return false;
    }
    initialized_ = true;
    configureScrollRegion();
    drawFooter();
    return true;
}

void TerminalLayout::setStatus(ConsoleStatus status)
{
    status_ = std::move(status);
}

void TerminalLayout::redrawFooter()
{
    if (initialized_)
    {
        drawFooter();
    }
}

void TerminalLayout::refreshAtSafeBoundary()
{
    if (!initialized_ || !updateSize())
    {
        return;
    }
    output_ << "\x1b[r";
    configureScrollRegion();
    drawFooter();
}

void TerminalLayout::restore()
{
    if (!initialized_)
    {
        return;
    }
    output_ << "\x1b[r\x1b[0m";
    if (updateSize())
    {
        output_ << "\x1b[" << rows_ << ";1H\x1b[2K";
    }
    output_.flush();
    initialized_ = false;
}

bool TerminalLayout::active() const noexcept
{
    return initialized_;
}

int TerminalLayout::columns() const noexcept
{
    return columns_;
}

bool TerminalLayout::updateSize()
{
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1 || size.ws_row < 2 || size.ws_col == 0)
    {
        return false;
    }
    rows_ = static_cast<int>(size.ws_row);
    columns_ = static_cast<int>(size.ws_col);
    return true;
}

void TerminalLayout::configureScrollRegion()
{
    output_ << "\x1b[1;" << (rows_ - 1) << "r";
}

void TerminalLayout::drawFooter()
{
    output_ << "\x1b[s"
            << "\x1b[" << rows_ << ";1H"
            << "\x1b[2K"
            << "\x1b[7m"
            << footer_renderer_.render(status_, columns_)
            << "\x1b[0m"
            << "\x1b[u";
    output_.flush();
}
