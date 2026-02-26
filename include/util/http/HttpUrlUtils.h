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

#include <string>

namespace mldp_pvxs_driver::util::http {

/**
 * @brief Small URL helper utilities shared by HTTP-facing components.
 *
 * These helpers are intentionally stateless.
 */
class HttpUrlUtils
{
public:
    /**
     * @brief Return true when the string contains a URL scheme (e.g. http://).
     */
    static bool hasScheme(const std::string& s);

    /**
     * @brief Remove trailing '/' characters from a URL/base path string.
     */
    static std::string trimTrailingSlash(std::string s);

    /**
     * @brief Percent-encode a string for use in URL query parameters.
     */
    static std::string percentEncode(const std::string& in);
};

} // namespace mldp_pvxs_driver::util::http
