//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <query/QueryProgress.h>
#include <query/QueryStats.h>

#include <optional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>

namespace mldp_pvxs_driver::cli {

struct ConsoleStatus
{
    bool                                 query_running{false};
    std::optional<query::QueryProgressSnapshot> progress;
    std::optional<query::QueryStats>     completed_stats;
    std::string                          error;
};

class FooterRenderer
{
public:
    std::string render(const ConsoleStatus& status, int terminal_width) const;
};

class TerminalLayout
{
public:
    explicit TerminalLayout(std::ostream& output, std::shared_ptr<std::mutex> output_mutex = nullptr);
    ~TerminalLayout();

    TerminalLayout(const TerminalLayout&) = delete;
    TerminalLayout& operator=(const TerminalLayout&) = delete;

    bool initialize();
    void setStatus(ConsoleStatus status);
    void redrawFooter();
    void refreshAtSafeBoundary();
    void positionInputCursor();
    void restore();

    bool active() const noexcept;
    int columns() const noexcept;

private:
    bool updateSize();
    void configureScrollRegion();
    void drawFooter();

    std::ostream&    output_;
    std::shared_ptr<std::mutex> output_mutex_;
    FooterRenderer   footer_renderer_;
    ConsoleStatus    status_;
    int              rows_{0};
    int              columns_{0};
    bool             initialized_{false};
};

} // namespace mldp_pvxs_driver::cli
