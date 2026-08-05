//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file ConsoleFooter.h
 * @brief Declares the active-query terminal progress footer. */
#pragma once

#include <query/QueryProgress.h>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>

namespace mldp_pvxs_driver::cli {

/** @brief Captures the latest query lifecycle state displayed in the footer. */
struct ConsoleStatus
{
    bool                                 query_running{false};  ///< True while a query is actively executing.
    std::optional<query::QueryProgressSnapshot> progress;       ///< Latest progress snapshot; empty when idle.
};

/** @brief Formats query lifecycle state into a terminal footer line. */
class FooterRenderer
{
public:
    /** @brief Renders the status footer as a single terminal line.
     * @param[in] status          Current query lifecycle status.
     * @param[in] terminal_width  Available columns for the footer line.
     * @return Formatted footer string, truncated to terminal_width if needed. */
    std::string render(const ConsoleStatus& status, int terminal_width) const;
};

/** @brief Temporarily reserves the final terminal row while a direct-output query runs. */
class TerminalLayout
{
public:
    /** @brief Constructs a terminal layout and registers the output stream.
     * @param[in,out] output        Output stream for ANSI escape sequences.
     * @param[in]     output_mutex  Optional mutex protecting concurrent writes to output. */
    explicit TerminalLayout(std::ostream& output, std::shared_ptr<std::mutex> output_mutex = nullptr);
    ~TerminalLayout();

    TerminalLayout(const TerminalLayout&) = delete;
    TerminalLayout& operator=(const TerminalLayout&) = delete;

    /** @brief Detects terminal dimensions and reserves the footer row.
     * @return True if a real TTY was detected and the footer was initialized. */
    bool initialize();

    /** @brief Redraws the footer with updated status without scrolling the main output.
     * @param[in] status  Latest query lifecycle status. */
    void redraw(const ConsoleStatus& status);

    /** @brief Restores the terminal to its original state, removing the scroll region. */
    void restore() noexcept;

private:
    bool updateSize();
    void configureScrollRegion();
    void draw(const ConsoleStatus& status);

    std::ostream&               output_;         ///< Output stream for ANSI escape sequences.
    std::shared_ptr<std::mutex> output_mutex_;   ///< Optional mutex for concurrent output access.
    FooterRenderer              footer_renderer_; ///< Stateless footer formatting helper.
    int                         rows_{0};         ///< Detected terminal row count.
    int                         columns_{0};      ///< Detected terminal column count.
    bool                        initialized_{false}; ///< True after a successful initialize() call.
};

} // namespace mldp_pvxs_driver::cli
