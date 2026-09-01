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

#include "../../../mock/MockArchiverPbHttpServer.h"

#include <EPICSEvent.pb.h>
#include <util/http/CurlHttpClient.h>
#include <util/http/HttpClient.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer;
using mldp_pvxs_driver::util::http::CurlHttpClient;
using mldp_pvxs_driver::util::http::HttpClientOptions;
using mldp_pvxs_driver::util::http::HttpRequest;

constexpr const char* kPath = mldp_pvxs_driver::reader::impl::epics_archiver::kMockArchiverPbHttpPath;
constexpr const char* kTestPv = mldp_pvxs_driver::reader::impl::epics_archiver::kMockArchiverPbHttpPv;

std::string unescapePbHttpLine(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(in[i]);
        if (ch != 0x1B)
        {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        if (i + 1 >= in.size())
        {
            throw std::runtime_error("truncated PB/HTTP escape sequence");
        }
        const unsigned char code = static_cast<unsigned char>(in[++i]);
        switch (code)
        {
        case 0x01: out.push_back(static_cast<char>(0x1B)); break;
        case 0x02: out.push_back('\n'); break;
        case 0x03: out.push_back('\r'); break;
        default:
            throw std::runtime_error("unexpected PB/HTTP escape code: " + std::to_string(static_cast<int>(code)));
        }
    }
    return out;
}

std::vector<std::string> splitLinesKeepEmpty(const std::string& body)
{
    std::vector<std::string> lines;
    std::string              current;
    for (char ch : body)
    {
        if (ch == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(ch);
        }
    }
    if (!current.empty())
    {
        lines.push_back(current);
    }
    return lines;
}

class ArchiverPbHttpMockServerTest : public ::testing::Test
{
protected:
    static MockArchiverPbHttpServer::GenerationConfig makeConfig()
    {
        MockArchiverPbHttpServer::GenerationConfig cfg;
        cfg.min_events_per_second = 4;
        cfg.max_events_per_second = 4; // deterministic count for tests
        cfg.open_ended_duration_sec = 1;
        cfg.min_value = -10.0;
        cfg.max_value = 10.0;
        cfg.random_seed = 123456u;
        return cfg;
    }

    ArchiverPbHttpMockServerTest()
        : server_(makeConfig())
    {
    }

    void SetUp() override
    {
        server_.start();
        ASSERT_GT(server_.port(), 0);

        HttpClientOptions options;
        options.connect_timeout_sec = 2;
        options.total_timeout_sec = 5;
        client_.setDefaultOptions(options);
        client_.setDefaultHeaders({"Accept: application/octet-stream"});
    }

    void TearDown() override
    {
        server_.stop();
    }

    MockArchiverPbHttpServer server_;
    CurlHttpClient           client_;
};

// Verifies the mock PB/HTTP server accepts a from-only query and returns a decodable PB/HTTP payload.
TEST_F(ArchiverPbHttpMockServerTest, SupportsFromOnlyQueryAndReturnsPbHttpBody)
{
    std::string collected;
    const auto  response = client_.streamGet(
        HttpRequest{
            .url = server_.baseUrl() + kPath + "?pv=TEST%3APV%3ADOUBLE&from=2026-02-25T08%3A00%3A00.000Z"},
        [&](const char* data, std::size_t size)
        {
            collected.append(data, size);
        });

    EXPECT_EQ(response.http_status, 200);
    EXPECT_FALSE(collected.empty());
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));

    const auto req = server_.lastRequest();
    ASSERT_TRUE(req.pv.has_value());
    ASSERT_TRUE(req.from.has_value());
    EXPECT_EQ(*req.pv, "TEST:PV:DOUBLE");
    EXPECT_EQ(*req.from, "2026-02-25T08:00:00.000Z");
    EXPECT_FALSE(req.to.has_value());

    const auto lines = splitLinesKeepEmpty(collected);
    ASSERT_GE(lines.size(), 6u);
    EXPECT_TRUE(lines.back().empty()); // Chunk terminator

    EPICS::PayloadInfo header;
    ASSERT_TRUE(header.ParseFromString(unescapePbHttpLine(lines[0])));
    EXPECT_EQ(header.type(), EPICS::SCALAR_DOUBLE);
    EXPECT_EQ(header.pvname(), kTestPv);
    EXPECT_EQ(header.elementcount(), 1);

    const auto&  cfg = server_.generationConfig();
    const size_t sample_count = lines.size() - 2; // header + empty chunk terminator
    EXPECT_GE(sample_count, static_cast<size_t>(cfg.min_events_per_second * cfg.open_ended_duration_sec));

    std::tuple<uint32_t, uint32_t> prev_ts{0u, 0u};
    bool                           first = true;
    for (size_t i = 1; i < lines.size() - 1; ++i)
    {
        EPICS::ScalarDouble sample;
        ASSERT_TRUE(sample.ParseFromString(unescapePbHttpLine(lines[i])));
        EXPECT_GE(sample.val(), cfg.min_value);
        EXPECT_LE(sample.val(), cfg.max_value);
        EXPECT_GE(sample.nano(), 0u);
        EXPECT_LT(sample.nano(), 1'000'000'000u);

        const auto ts = std::make_tuple(sample.secondsintoyear(), sample.nano());
        if (!first)
        {
            EXPECT_LE(prev_ts, ts);
        }
        prev_ts = ts;
        first = false;
    }
}

// Verifies the mock derives the PB/HTTP payload type from the requested PV suffix.
TEST_F(ArchiverPbHttpMockServerTest, SelectsPayloadTypeFromRequestedPvSuffix)
{
    std::string collected;
    const auto  response = client_.streamGet(
        HttpRequest{
            .url = server_.baseUrl() + kPath + "?pv=TEST%3APV_WAVEFORM_DOUBLE&from=2026-02-25T08%3A00%3A00.000Z"},
        [&](const char* data, std::size_t size)
        {
            collected.append(data, size);
        });

    EXPECT_EQ(response.http_status, 200);
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));

    const auto lines = splitLinesKeepEmpty(collected);
    ASSERT_GE(lines.size(), 6u);

    EPICS::PayloadInfo header;
    ASSERT_TRUE(header.ParseFromString(unescapePbHttpLine(lines[0])));
    EXPECT_EQ(header.type(), EPICS::WAVEFORM_DOUBLE);
    EXPECT_EQ(header.pvname(), "TEST:PV_WAVEFORM_DOUBLE");
    EXPECT_EQ(header.elementcount(), 4);

    EPICS::VectorDouble sample;
    ASSERT_TRUE(sample.ParseFromString(unescapePbHttpLine(lines[1])));
    ASSERT_EQ(sample.val_size(), 4);
    EXPECT_NE(sample.val(0), sample.val(1));
}

// Verifies the mock PB/HTTP server accepts and records the optional 'to' query parameter.
TEST_F(ArchiverPbHttpMockServerTest, AcceptsOptionalToQuery)
{
    const auto result = client_.get(HttpRequest{
        .url = server_.baseUrl() + kPath + "?pv=TEST%3APV%3ADOUBLE&from=2026-02-25T08%3A00%3A00.000Z" "&to=2026-02-25T08%3A00%3A02.000Z"});

    EXPECT_EQ(result.info.http_status, 200);
    EXPECT_FALSE(result.body.empty());
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));

    const auto req = server_.lastRequest();
    ASSERT_TRUE(req.to.has_value());
    EXPECT_EQ(*req.to, "2026-02-25T08:00:02.000Z");
}

// Verifies the mock chooses the response payload family from the requested PV suffix.
TEST_F(ArchiverPbHttpMockServerTest, SupportsTypedPayloadSelectionFromPvSuffix)
{
    std::string collected;
    const auto  response = client_.streamGet(
        HttpRequest{
            .url = server_.baseUrl() + kPath + "?pv=TEST%3APV_STRING_SCALAR_STRING&from=2026-02-25T08%3A00%3A00.000Z"},
        [&](const char* data, std::size_t size)
        {
            collected.append(data, size);
        });

    EXPECT_EQ(response.http_status, 200);
    EXPECT_FALSE(collected.empty());
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));

    const auto lines = splitLinesKeepEmpty(collected);
    ASSERT_GE(lines.size(), 6u);

    EPICS::PayloadInfo header;
    ASSERT_TRUE(header.ParseFromString(unescapePbHttpLine(lines[0])));
    EXPECT_EQ(header.type(), EPICS::SCALAR_STRING);
    EXPECT_EQ(header.pvname(), "TEST:PV_STRING_SCALAR_STRING");

    EPICS::ScalarString sample;
    ASSERT_TRUE(sample.ParseFromString(unescapePbHttpLine(lines[1])));
    EXPECT_FALSE(sample.val().empty());
}

// Verifies requests missing the required pv parameter return HTTP 400.
TEST_F(ArchiverPbHttpMockServerTest, MissingPvReturns400)
{
    const auto result = client_.get(HttpRequest{
        .url = server_.baseUrl() + kPath + "?from=2026-02-25T08%3A00%3A00.000Z"});

    EXPECT_EQ(result.info.http_status, 400);
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));
    const std::string body(result.body.begin(), result.body.end());
    EXPECT_NE(body.find("pv"), std::string::npos);
}

// Verifies requests missing the required from parameter return HTTP 400.
TEST_F(ArchiverPbHttpMockServerTest, MissingFromReturns400)
{
    const auto result = client_.get(HttpRequest{.url = server_.baseUrl() + kPath + "?pv=TEST%3APV%3ADOUBLE"});

    EXPECT_EQ(result.info.http_status, 400);
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));
    const std::string body(result.body.begin(), result.body.end());
    EXPECT_NE(body.find("from"), std::string::npos);
}

// Verifies unsupported paths return HTTP 404 from the mock server.
TEST_F(ArchiverPbHttpMockServerTest, WrongPathReturns404)
{
    const auto result = client_.get(HttpRequest{
        .url = server_.baseUrl() + "/retrieval/data/notFound.raw?pv=TEST%3APV%3ADOUBLE&from=2026-02-25T08%3A00%3A00.000Z"});

    EXPECT_EQ(result.info.http_status, 404);
    ASSERT_TRUE(server_.waitForLastResponseComplete(std::chrono::seconds(2)));
}

} // namespace
