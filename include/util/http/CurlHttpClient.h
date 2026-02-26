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

#include <util/http/HttpClient.h>
#include <util/log/ILog.h>

#include <atomic>
#include <memory>

namespace mldp_pvxs_driver::util::http {

/**
 * @file CurlHttpClient.h
 * @brief libcurl-backed implementation of the util/http transport interface.
 */

/**
 * @brief HTTP client implementation backed by libcurl.
 *
 * This implementation centralizes common transport concerns used by readers:
 * timeouts, TLS verification, redirects, compression, request headers, and
 * streaming callbacks.
 */
class CurlHttpClient final : public IHttpClient
{
public:
    /**
     * @brief Construct a curl-backed HTTP client.
     *
     * @param logger Optional logger for transport diagnostics. If omitted, a
     *        named logger is created via the project's logging factory.
     */
    explicit CurlHttpClient(std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger = {});
    ~CurlHttpClient() override = default;

    /// @copydoc IHttpClient::setDefaultOptions
    void setDefaultOptions(const HttpClientOptions& options) override;
    /// @copydoc IHttpClient::setDefaultHeaders
    void setDefaultHeaders(std::vector<std::string> headers) override;

    /// @copydoc IHttpClient::streamGet
    HttpResponseInfo streamGet(const HttpRequest& request, DataCallback onData) override;
    /// @copydoc IHttpClient::cancelOngoingRequests
    void cancelOngoingRequests() override;
    /// @copydoc IHttpClient::get
    HttpGetResult get(const HttpRequest& request) override;

private:
    std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> logger_;                  ///< Transport logger.
    HttpClientOptions                                     options_;                 ///< Default request/transport options.
    std::vector<std::string>                              default_headers_;         ///< Default headers merged into requests.
    std::atomic<bool>                                     cancel_requested_{false}; ///< Cooperative cancel flag checked by libcurl callbacks.
};

} // namespace mldp_pvxs_driver::util::http
