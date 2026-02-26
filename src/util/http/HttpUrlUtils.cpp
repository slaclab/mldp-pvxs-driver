//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <util/http/HttpUrlUtils.h>

#include <cctype>
#include <iomanip>
#include <sstream>

namespace mldp_pvxs_driver::util::http {

bool HttpUrlUtils::hasScheme(const std::string& s)
{
    return s.find("://") != std::string::npos;
}

std::string HttpUrlUtils::trimTrailingSlash(std::string s)
{
    while (!s.empty() && s.back() == '/')
    {
        s.pop_back();
    }
    return s;
}

std::string HttpUrlUtils::percentEncode(const std::string& in)
{
    std::ostringstream os;
    os << std::uppercase << std::hex;
    for (unsigned char c : in)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            os << static_cast<char>(c);
        }
        else
        {
            os << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return os.str();
}

} // namespace mldp_pvxs_driver::util::http
