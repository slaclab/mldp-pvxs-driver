//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <util/http/CurlHttpClient.h>

#include <util/log/Logger.h>

#include <curl/curl.h>

#include <exception>
#include <memory>
#include <mutex>

#include <string>
#include <utility>
#include <vector>

using namespace mldp_pvxs_driver::util::http;
using namespace mldp_pvxs_driver::util::log;

namespace {

std::once_flag g_curl_global_init_once;

void ensureCurlGlobalInit()
{
    std::call_once(g_curl_global_init_once, []()
                   {
                       const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
                       if (rc != CURLE_OK)
                       {
                           throw HttpError(std::string("curl_global_init failed: ") + curl_easy_strerror(rc));
                       }
                   });
}

class CurlHandleDeleter
{
public:
    void operator()(CURL* h) const noexcept
    {
        if (h)
        {
            curl_easy_cleanup(h);
        }
    }
};

using CurlHandlePtr = std::unique_ptr<CURL, CurlHandleDeleter>;

class CurlSlistHolder
{
public:
    CurlSlistHolder() = default;
    CurlSlistHolder(const CurlSlistHolder&) = delete;
    CurlSlistHolder& operator=(const CurlSlistHolder&) = delete;

    ~CurlSlistHolder()
    {
        if (list_)
        {
            curl_slist_free_all(list_);
        }
    }

    void append(const std::string& header)
    {
        curl_slist* next = curl_slist_append(list_, header.c_str());
        if (!next)
        {
            throw HttpError("curl_slist_append failed");
        }
        list_ = next;
    }

    curl_slist* get() const noexcept
    {
        return list_;
    }

private:
    curl_slist* list_ = nullptr;
};

template <typename T>
void setoptChecked(CURL* curl, CURLoption option, T value)
{
    const CURLcode rc = curl_easy_setopt(curl, option, value);
    if (rc != CURLE_OK)
    {
        throw HttpError(std::string("curl_easy_setopt failed: ") + curl_easy_strerror(rc));
    }
}

struct WriteCallbackContext
{
    IHttpClient::DataCallback onData;
    std::exception_ptr        callback_error;
};

int onProgress(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    const auto* cancel_requested = static_cast<const std::atomic<bool>*>(clientp);
    return (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) ? 1 : 0;
}

size_t onWrite(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const size_t bytes = size * nmemb;
    if (bytes == 0)
    {
        return 0;
    }

    auto* ctx = static_cast<WriteCallbackContext*>(userdata);
    try
    {
        ctx->onData(ptr, bytes);
        return bytes;
    }
    catch (...)
    {
        ctx->callback_error = std::current_exception();
        return 0; // Abort transfer; error is rethrown by streamGet().
    }
}

} // namespace

CurlHttpClient::CurlHttpClient(std::shared_ptr<ILogger> logger)
    : logger_(std::move(logger))
{
    if (!logger_)
    {
        logger_ = newLogger("util:http:curl");
    }

    ensureCurlGlobalInit();
}

void CurlHttpClient::setDefaultOptions(const HttpClientOptions& options)
{
    if (options.tls.verify_host && !options.tls.verify_peer)
    {
        throw HttpError("TLS host verification requires peer verification");
    }

    options_ = options;
    debugf(
        *logger_,
        "Configured CurlHttpClient defaults: connect_timeout={}s total_timeout={}s tls(peer={},host={})",
        options_.connect_timeout_sec,
        options_.total_timeout_sec,
        options_.tls.verify_peer,
        options_.tls.verify_host);
}

void CurlHttpClient::setDefaultHeaders(std::vector<std::string> headers)
{
    default_headers_ = std::move(headers);
}

HttpResponseInfo CurlHttpClient::streamGet(const HttpRequest& request, DataCallback onData)
{
    if (request.url.empty())
    {
        throw HttpError("HTTP request url must not be empty");
    }
    if (!onData)
    {
        throw HttpError("HTTP streamGet requires a data callback");
    }
    if (options_.tls.verify_host && !options_.tls.verify_peer)
    {
        throw HttpError("TLS host verification requires peer verification");
    }

    if (cancel_requested_.load(std::memory_order_relaxed))
    {
        throw HttpError("HTTP request cancelled");
    }

    CurlHandlePtr curl(curl_easy_init());
    if (!curl)
    {
        throw HttpError("curl_easy_init failed");
    }

    CurlSlistHolder headers;
    for (const auto& h : default_headers_)
    {
        headers.append(h);
    }
    for (const auto& h : request.headers)
    {
        headers.append(h);
    }

    WriteCallbackContext write_ctx{std::move(onData)};

    setoptChecked(curl.get(), CURLOPT_NOSIGNAL, 1L);
    setoptChecked(curl.get(), CURLOPT_HTTPGET, 1L);
    setoptChecked(curl.get(), CURLOPT_URL, request.url.c_str());
    setoptChecked(curl.get(), CURLOPT_WRITEFUNCTION, &onWrite);
    setoptChecked(curl.get(), CURLOPT_WRITEDATA, &write_ctx);
    setoptChecked(curl.get(), CURLOPT_NOPROGRESS, 0L);
    setoptChecked(curl.get(), CURLOPT_XFERINFOFUNCTION, &onProgress);
    setoptChecked(curl.get(), CURLOPT_XFERINFODATA, &cancel_requested_);

    setoptChecked(curl.get(), CURLOPT_CONNECTTIMEOUT, options_.connect_timeout_sec);
    setoptChecked(curl.get(), CURLOPT_TIMEOUT, options_.total_timeout_sec);
    setoptChecked(curl.get(), CURLOPT_LOW_SPEED_LIMIT, options_.low_speed_limit_bytes_per_sec);
    setoptChecked(curl.get(), CURLOPT_LOW_SPEED_TIME, options_.low_speed_time_sec);

    setoptChecked(curl.get(), CURLOPT_SSL_VERIFYPEER, options_.tls.verify_peer ? 1L : 0L);
    setoptChecked(curl.get(), CURLOPT_SSL_VERIFYHOST, options_.tls.verify_host ? 2L : 0L);

    setoptChecked(curl.get(), CURLOPT_FOLLOWLOCATION, options_.follow_redirects ? 1L : 0L);
    setoptChecked(curl.get(), CURLOPT_MAXREDIRS, options_.max_redirects);

    if (options_.tcp_keepalive)
    {
        setoptChecked(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
#ifdef CURLOPT_TCP_KEEPIDLE
        setoptChecked(curl.get(), CURLOPT_TCP_KEEPIDLE, options_.tcp_keepidle_sec);
#endif
#ifdef CURLOPT_TCP_KEEPINTVL
        setoptChecked(curl.get(), CURLOPT_TCP_KEEPINTVL, options_.tcp_keepintvl_sec);
#endif
    }
    else
    {
        setoptChecked(curl.get(), CURLOPT_TCP_KEEPALIVE, 0L);
    }

    if (options_.buffer_size > 0)
    {
        setoptChecked(curl.get(), CURLOPT_BUFFERSIZE, options_.buffer_size);
    }

    if (!options_.user_agent.empty())
    {
        setoptChecked(curl.get(), CURLOPT_USERAGENT, options_.user_agent.c_str());
    }

    if (options_.enable_compression)
    {
        setoptChecked(curl.get(), CURLOPT_ACCEPT_ENCODING, options_.accept_encoding.c_str());
    }
    else
    {
        setoptChecked(curl.get(), CURLOPT_ACCEPT_ENCODING, "identity");
    }

    if (headers.get())
    {
        setoptChecked(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    }

    debugf(*logger_, "HTTP GET stream start: {}", request.url);

    const CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK)
    {
        if (write_ctx.callback_error && rc == CURLE_WRITE_ERROR)
        {
            std::rethrow_exception(write_ctx.callback_error);
        }
        if (rc == CURLE_ABORTED_BY_CALLBACK && cancel_requested_.load(std::memory_order_relaxed))
        {
            debugf(*logger_, "HTTP GET stream cancelled: {}", request.url);
            throw HttpError("HTTP request cancelled");
        }
        errorf(
            *logger_, "HTTP GET stream failed for {}: {}", request.url, curl_easy_strerror(rc));
        throw HttpError(std::string("curl_easy_perform failed: ") + curl_easy_strerror(rc));
    }

    HttpResponseInfo info{};
    char*            effective_url = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &info.http_status);
    if (curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK && effective_url)
    {
        info.effective_url = effective_url;
    }

    debugf(
        *logger_,
        "HTTP GET stream completed: status={} effective_url={}",
        info.http_status,
        info.effective_url.empty() ? request.url : info.effective_url);

    return info;
}

void CurlHttpClient::cancelOngoingRequests()
{
    cancel_requested_.store(true, std::memory_order_relaxed);
}

HttpGetResult CurlHttpClient::get(const HttpRequest& request)
{
    HttpGetResult result;
    result.body.reserve(static_cast<std::size_t>(options_.buffer_size > 0 ? options_.buffer_size : 0));

    result.info = streamGet(request, [&](const char* data, std::size_t size)
                            {
                                result.body.insert(result.body.end(), data, data + size);
                            });

    return result;
}
