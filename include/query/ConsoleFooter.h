//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
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
    bool                                 query_running{false};
    std::optional<query::QueryProgressSnapshot> progress;
};

/** @brief Formats query lifecycle state into a terminal footer line. */
class FooterRenderer
{
public:
    std::string render(const ConsoleStatus& status, int terminal_width) const;
};

/** @brief Temporarily reserves the final terminal row while a direct-output query runs. */
class TerminalLayout
{
public:
    explicit TerminalLayout(std::ostream& output, std::shared_ptr<std::mutex> output_mutex = nullptr);
    ~TerminalLayout();

    TerminalLayout(const TerminalLayout&) = delete;
    TerminalLayout& operator=(const TerminalLayout&) = delete;

    bool initialize();
    void redraw(const ConsoleStatus& status);
    void restore() noexcept;

private:
    bool updateSize();
    void configureScrollRegion();
    void draw(const ConsoleStatus& status);

    std::ostream&              output_;
    std::shared_ptr<std::mutex> output_mutex_;
    FooterRenderer             footer_renderer_;
    int                        rows_{0};
    int                        columns_{0};
    bool                       initialized_{false};
};

} // namespace mldp_pvxs_driver::cli
