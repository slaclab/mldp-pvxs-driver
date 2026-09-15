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

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::util::http {

/**
 * @file HttpClient.h
 * @brief HTTP transport abstraction and common request/response option types.
 *
 * This header defines a backend-agnostic HTTP client interface used by readers
 * and utility components. Concrete implementations (for example libcurl) are
 * provided in separate headers/source files.
 */

/**
 * @brief TLS verification settings for HTTPS connections.
 */
struct TlsConfig
{
    bool verify_peer = true; ///< Verify the server certificate chain.
    bool verify_host = true; ///< Verify the hostname against the certificate.
};

/**
 * @brief Shared HTTP client defaults applied to requests.
 *
 * These options represent transport-level behavior (timeouts, redirects, TLS,
 * compression, keepalive) and are intentionally protocol-agnostic.
 */
struct HttpClientOptions
{
    long connect_timeout_sec = 30L; ///< Connection establishment timeout in seconds.
    long total_timeout_sec = 300L;  ///< Total operation timeout in seconds (0 = infinite).

    long low_speed_limit_bytes_per_sec = 1024L; ///< Stall threshold in bytes/sec.
    long low_speed_time_sec = 60L;              ///< Stall duration before abort.

    bool tcp_keepalive = true;    ///< Enable TCP keepalive probes.
    long tcp_keepidle_sec = 120L; ///< Idle time before first keepalive probe.
    long tcp_keepintvl_sec = 60L; ///< Interval between keepalive probes.

    long buffer_size = 65536L; ///< Preferred receive buffer size hint (bytes).

    bool follow_redirects = true; ///< Follow HTTP redirects automatically.
    long max_redirects = 5L;      ///< Maximum redirects when following is enabled.

    bool        enable_compression = true;         ///< Enable response compression.
    std::string accept_encoding = "gzip, deflate"; ///< Accepted encodings if compression is enabled.

    std::string user_agent = "mldp-pvxs-driver/http"; ///< HTTP User-Agent string.
    TlsConfig   tls;                                  ///< TLS verification behavior.
};

/**
 * @brief HTTP request parameters for a GET operation.
 */
struct HttpRequest
{
    std::string              url;     ///< Absolute request URL.
    std::vector<std::string> headers; ///< Additional request headers ("Key: Value").
};

/**
 * @brief Minimal response metadata returned by transport operations.
 */
struct HttpResponseInfo
{
    long        http_status = 0; ///< HTTP status code if available (0 if unavailable).
    std::string effective_url;   ///< Final URL after redirects (if provided by backend).
};

/**
 * @brief Buffered result for a synchronous GET.
 */
struct HttpGetResult
{
    HttpResponseInfo  info; ///< Response metadata (status/effective URL).
    std::vector<char> body; ///< Full response body bytes.
};

/**
 * @brief Transport-level HTTP exception.
 */
class HttpError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Backend-agnostic HTTP client interface.
 *
 * Implementations are expected to be thread-safe only when documented as such.
 * Callers should assume an instance is not safe for concurrent mutation unless
 * the concrete implementation guarantees it.
 */
class IHttpClient
{
public:
    /**
     * @brief Callback invoked with raw response bytes for streaming GET requests.
     *
     * The callback receives arbitrary byte chunks. Chunk boundaries are transport
     * callback boundaries and do not imply protocol message/frame boundaries.
     */
    using DataCallback = std::function<void(const char* data, std::size_t size)>;

    virtual ~IHttpClient() = default;

    /**
     * @brief Set default transport options used for subsequent requests.
     */
    virtual void setDefaultOptions(const HttpClientOptions& options) = 0;

    /**
     * @brief Set default request headers applied to subsequent requests.
     *
     * Per-request headers may be appended by the caller via @ref HttpRequest.
     */
    virtual void setDefaultHeaders(std::vector<std::string> headers) = 0;

    /**
     * @brief Perform a streaming HTTP GET.
     *
     * @param request GET request details.
     * @param onData Callback invoked with response body chunks.
     * @return Response metadata (status, effective URL).
     * @throws HttpError on transport/configuration failures.
     */
    virtual HttpResponseInfo streamGet(const HttpRequest& request, DataCallback onData) = 0;

    /**
     * @brief Request cancellation of any ongoing transport operations.
     *
     * Implementations should make a best effort to interrupt blocking network
     * calls promptly. Safe to call when no request is active.
     */
    virtual void cancelOngoingRequests() = 0;

    /**
     * @brief Synchronous GET convenience helper that buffers the full response body.
     *
     * @param request GET request details.
     * @return Buffered body bytes plus response metadata.
     * @throws HttpError on transport/configuration failures.
     */
    virtual HttpGetResult get(const HttpRequest& request) = 0;
};

/**
 * @brief Owning pointer type for HTTP client implementations.
 */
using IHttpClientPtr = std::unique_ptr<IHttpClient>;

} // namespace mldp_pvxs_driver::util::http
