//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include <util/http/CurlHttpClient.h>
#include <util/http/HttpClient.h>

namespace {

using mldp_pvxs_driver::util::http::CurlHttpClient;
using mldp_pvxs_driver::util::http::HttpClientOptions;
using mldp_pvxs_driver::util::http::HttpError;
using mldp_pvxs_driver::util::http::HttpRequest;

// Verifies the client constructor always supplies a logger, so methods can log
// unconditionally even when no logger is provided by the caller.
TEST(CurlHttpClientTest, DefaultConstructedClientSupportsLoggingCallsWithoutInjectedLogger)
{
    CurlHttpClient client;

    HttpClientOptions options;
    options.connect_timeout_sec = 1;
    options.total_timeout_sec = 1;
    client.setDefaultOptions(options);
    client.setDefaultHeaders({"Accept: application/octet-stream"});

    EXPECT_THROW(
        (void)client.get(HttpRequest{.url = "http://127.0.0.1:1/unreachable"}),
        HttpError);
}

} // namespace
