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

#include <reader/IReaderLifecycle.h>
#include <reader/impl/slac_calendar/SlacCalendarReader.h>
#include <util/bus/IDataBus.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../../../config/test_config_helpers.h"
#include "../../../mock/MockCalendarHttpServer.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::slac_calendar::SlacCalendarReader;
using mldp_pvxs_driver::test::mock::MockCalendarHttpServer;
using mldp_pvxs_driver::util::bus::ConfigurationActivationPayload;
using mldp_pvxs_driver::util::bus::ConfigurationPayload;
using mldp_pvxs_driver::util::bus::IDataBus;

namespace {

// ---------------------------------------------------------------------------
// Capturing bus
// ---------------------------------------------------------------------------

class CapturingBus final : public IDataBus
{
public:
    bool push(EventBatch batch) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        batches_.push_back(std::move(batch));
        count_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_all();
        return true;
    }

    std::vector<EventBatch> snapshot() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return batches_;
    }

    bool waitForCount(int target, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] {
            return count_.load(std::memory_order_relaxed) >= target;
        });
    }

    std::atomic<int> count_{0};

private:
    mutable std::mutex              mu_;
    mutable std::condition_variable cv_;
    std::vector<EventBatch>         batches_;
};

class LifecycleObserver final : public mldp_pvxs_driver::reader::IReaderLifecycle
{
public:
    void onReaderCompleted(const std::string& reader_name) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        completed_name_ = reader_name;
        ++count_;
        cv_.notify_all();
    }

    bool waitForCompletion(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return count_ > 0; });
    }

    int completionCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return count_;
    }

    std::string completedName() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return completed_name_;
    }

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    int                     count_{0};
    std::string             completed_name_;
};

// ---------------------------------------------------------------------------
// LCLS sample event JSON
// ---------------------------------------------------------------------------

static const char* kLclsEvent = R"json([
  {
    "url": "https://www.google.com/calendar/event?eid=abc123",
    "program_name": "CXI 1013443 Bain",
    "description": "Deliver to CXI",
    "note": "13.213 GeV, 80 pC",
    "calendar": "NC-CXI",
    "details": "<a href=\"https://pswww.slac.stanford.edu/foo\">https://pswww.slac.stanford.edu/foo</a>",
    "start": "2026-05-28T06:00:00-07:00",
    "end": "2026-05-28T18:00:00-07:00",
    "tags": ["2nd"],
    "poc": "Minitti",
    "config": "15 keV",
    "hutch": {"name": "CXI", "color": "#a00000", "line": "HXR", "text_color": "white"},
    "machine": "NC"
  }
])json";

// ---------------------------------------------------------------------------
// FACET sample event JSON (reduced schema)
// ---------------------------------------------------------------------------

static const char* kFacetEvent = R"json([
  {
    "url": "https://www.google.com/calendar/event?eid=facet001",
    "program_name": "Single bunch matching S20",
    "description": "",
    "note": null,
    "calendar": "FACET-MD",
    "details": null,
    "start": "2026-05-28T12:00:00-07:00",
    "end": "2026-05-28T14:00:00-07:00"
  }
])json";

// ---------------------------------------------------------------------------
// Build reader YAML
// ---------------------------------------------------------------------------

std::string makeReaderYaml(const std::string& baseUrl,
                           const std::string& experiments,
                           int                lookahead = 30)
{
    std::ostringstream ss;
    ss << "name: test-cal-reader\n"
       << "base-url: " << baseUrl << "\n"
       << "experiments:\n"
       << experiments
       << "lookahead-days: " << lookahead << "\n"
       << "lookback-days: 1\n"
       << "rescan-interval-sec: 0.0\n"
       << "connect-timeout-sec: 5\n"
       << "total-timeout-sec: 15\n"
       << "tls-verify-peer: false\n"
       << "tls-verify-host: false\n";
    return ss.str();
}

} // namespace

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SlacCalendarReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server_.start();
        bus_ = std::make_shared<CapturingBus>();
    }

    void TearDown() override
    {
        server_.stop();
    }

    MockCalendarHttpServer     server_;
    std::shared_ptr<CapturingBus> bus_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(SlacCalendarReaderTest, LclsEventProducesTwoBusMessages)
{
    server_.setResponse("lcls", kLclsEvent);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    ASSERT_EQ(batches.size(), 2u);

    // First batch: ConfigurationPayload
    ASSERT_TRUE(std::holds_alternative<ConfigurationPayload>(batches[0].payload));
    const auto& cp = std::get<ConfigurationPayload>(batches[0].payload);
    EXPECT_EQ(cp.configuration_name, "CXI 1013443 Bain");
    EXPECT_EQ(cp.category, "NC-CXI");
    ASSERT_TRUE(cp.description.has_value());
    EXPECT_EQ(*cp.description, "Deliver to CXI");
    ASSERT_TRUE(cp.tags.has_value());
    ASSERT_EQ(cp.tags->size(), 1u);
    EXPECT_EQ((*cp.tags)[0], "2nd");
    EXPECT_EQ(cp.attributes.at("tag_0"), "2nd");
    EXPECT_EQ(cp.attributes.at("experiment"), "lcls");
    EXPECT_EQ(cp.attributes.at("note"), "13.213 GeV, 80 pC");
    EXPECT_EQ(cp.attributes.at("poc"), "Minitti");
    EXPECT_EQ(cp.attributes.at("config"), "15 keV");
    EXPECT_EQ(cp.attributes.at("machine"), "NC");
    EXPECT_EQ(cp.attributes.at("hutch_name"), "CXI");
    EXPECT_EQ(cp.attributes.at("hutch_color"), "#a00000");
    EXPECT_EQ(cp.attributes.at("hutch_line"), "HXR");
    EXPECT_FALSE(cp.attributes.count("text_color"));

    // details should be inner-text of the anchor
    ASSERT_TRUE(cp.attributes.count("details"));
    EXPECT_EQ(cp.attributes.at("details"), "https://pswww.slac.stanford.edu/foo");

    // Second batch: ConfigurationActivationPayload
    ASSERT_TRUE(std::holds_alternative<ConfigurationActivationPayload>(batches[1].payload));
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    EXPECT_EQ(act.configuration_name, "CXI 1013443 Bain");
    ASSERT_TRUE(act.client_activation_id.has_value());
    EXPECT_EQ(*act.client_activation_id, "https://www.google.com/calendar/event?eid=abc123");
    EXPECT_EQ(act.start_time.epoch_seconds, static_cast<uint64_t>(1779973200));
    EXPECT_EQ(act.attributes.at("experiment"), "lcls");
    EXPECT_EQ(act.attributes.at("calendar"), "NC-CXI");
}

TEST_F(SlacCalendarReaderTest, FacetEventHandlesReducedSchema)
{
    server_.setResponse("facet", kFacetEvent);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - facet\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    ASSERT_EQ(batches.size(), 2u);

    const auto& cp = std::get<ConfigurationPayload>(batches[0].payload);
    EXPECT_EQ(cp.configuration_name, "Single bunch matching S20");
    EXPECT_FALSE(cp.description.has_value());
    EXPECT_FALSE(cp.tags.has_value());
    EXPECT_FALSE(cp.attributes.count("note"));
    EXPECT_FALSE(cp.attributes.count("poc"));
    EXPECT_FALSE(cp.attributes.count("config"));
    EXPECT_FALSE(cp.attributes.count("machine"));
    EXPECT_FALSE(cp.attributes.count("hutch_name"));
    EXPECT_EQ(cp.attributes.at("experiment"), "facet");
}

TEST_F(SlacCalendarReaderTest, MultipleExperimentsAllFetched)
{
    server_.setResponse("lcls", kLclsEvent);
    server_.setResponse("facet", kFacetEvent);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n  - facet\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    // 2 payloads per event × 2 experiments = 4 total
    ASSERT_TRUE(bus_->waitForCount(4, std::chrono::milliseconds(5000)));

    ASSERT_TRUE(server_.waitForRequestCount(2, std::chrono::milliseconds(3000)));
    const auto history = server_.requestHistory();
    ASSERT_EQ(history.size(), 2u);

    bool found_lcls  = false;
    bool found_facet = false;
    for (const auto& h : history)
    {
        if (h.experiment == "lcls")  found_lcls  = true;
        if (h.experiment == "facet") found_facet = true;
    }
    EXPECT_TRUE(found_lcls);
    EXPECT_TRUE(found_facet);
}

TEST_F(SlacCalendarReaderTest, OneShotRunSignalsLifecycleCompletion)
{
    server_.setResponse("lcls", kLclsEvent);

    const auto cfg = makeConfigFromYaml(makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    auto observer = std::make_shared<LifecycleObserver>();
    auto reader = std::make_unique<SlacCalendarReader>(bus_, nullptr, cfg);
    reader->setLifecycleObserver(observer);

    ASSERT_TRUE(observer->waitForCompletion(std::chrono::milliseconds(5000)));
    EXPECT_EQ(observer->completionCount(), 1);
    EXPECT_EQ(observer->completedName(), "test-cal-reader");
}

TEST_F(SlacCalendarReaderTest, HttpErrorIsLoggedAndSkipped)
{
    server_.setResponse("lcls", kLclsEvent);
    server_.setStatusCode("lcls", 500);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    // Should not throw; error is swallowed gracefully.
    ASSERT_NO_THROW({
        SlacCalendarReader reader(bus_, nullptr, cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });
    EXPECT_EQ(bus_->count_.load(), 0);
}

TEST_F(SlacCalendarReaderTest, UrlContainsEncodedTimestamps)
{
    server_.setResponse("lcls", "[]");

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(server_.waitForRequestCount(1, std::chrono::milliseconds(3000)));
    const auto history = server_.requestHistory();
    ASSERT_FALSE(history.empty());

    const auto& path = history[0].path;
    EXPECT_NE(path.find("lcls/events.json"), std::string::npos);
    // colons should be percent-encoded
    EXPECT_EQ(path.find(':'), std::string::npos);
}
