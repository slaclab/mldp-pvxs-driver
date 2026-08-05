//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryPager.h
 * @brief Declares optional terminal-pager support for REPL table output. */
#pragma once

#include <query/QueryFormatter.h>

#include <iosfwd>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::cli {

/** @brief Sends explicitly requested real-terminal table output to a pager. */
class QueryPager
{
public:
    /** @brief Returns true if the given streams connect to a real TTY and the format is table.
     * @param[in] input   Input stream to test.
     * @param[in] output  Output stream to test.
     * @param[in] format  Active output format.
     * @return True when paging is appropriate. */
    bool canPage(const std::istream& input, const std::ostream& output, QueryOutputFormat format) const noexcept;

    /** @brief Returns the system pager command string (e.g. "less -R").
     * @return Pager command. */
    std::string command() const;

    /** @brief Pipes text through the system pager.
     * @param[in]  text   Text to display.
     * @param[out] error  Receives an error message on failure.
     * @return True on success. */
    bool write(std::string_view text, std::string& error) const;
};

} // namespace mldp_pvxs_driver::cli
