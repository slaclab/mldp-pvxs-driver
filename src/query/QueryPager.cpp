//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryPager.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <unistd.h>

using namespace mldp_pvxs_driver::cli;

bool QueryPager::canPage(const std::istream& input, const std::ostream& output, const QueryOutputFormat format) const noexcept
{
    return &input == &std::cin && &output == &std::cout && ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0 &&
           format == QueryOutputFormat::Table;
}

std::string QueryPager::command() const
{
    const auto* pager = std::getenv("PAGER");
    return pager != nullptr && *pager != '\0' ? pager : "less -FRSX";
}

bool QueryPager::write(const std::string_view text, std::string& error) const
{
    FILE* const pipe = ::popen(command().c_str(), "w");
    if (pipe == nullptr)
    {
        error = "cannot start pager '" + command() + "': " + std::strerror(errno);
        return false;
    }
    const auto written = std::fwrite(text.data(), 1, text.size(), pipe);
    const auto status = ::pclose(pipe);
    if (written != text.size() || status != 0)
    {
        error = "pager '" + command() + "' did not accept the complete result";
        return false;
    }
    return true;
}
