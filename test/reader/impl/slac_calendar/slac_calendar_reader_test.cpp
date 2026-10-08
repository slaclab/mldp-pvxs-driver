//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <map>
#include <tuple>
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
                           const std::string& accels,
                           int                lookahead = 30)
{
    std::ostringstream ss;
    ss << "name: test-cal-reader\n"
       << "base-url: " << baseUrl << "\n"
       << "accel:\n"
       << accels
       << "lookahead-days: " << lookahead << "\n"
       << "lookback-days: 1\n"
       // Keep the whole [lookback, lookahead] span in a single HTTP window so the mock
       // server (which does not filter by date) isn't hit multiple times for one event.
       << "fetch-window-days: " << (lookahead + 2) << "\n"
       << "rescan-interval-sec: 0.0\n"
       << "connect-timeout-sec: 5\n"
       << "total-timeout-sec: 15\n"
       << "tls-verify-peer: false\n"
       << "tls-verify-host: false\n";
    return ss.str();
}

std::string makeReaderYamlWithCategory(const std::string& baseUrl,
                                       const std::string& accels,
                                       const std::string& category,
                                       int                lookahead = 30)
{
    return makeReaderYaml(baseUrl, accels, lookahead) +
           "category: \"" + category + "\"\n";
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
    EXPECT_EQ(cp.category, "CXI 1013443 Bain");
    ASSERT_TRUE(cp.description.has_value());
    EXPECT_EQ(*cp.description, "Deliver to CXI");
    ASSERT_TRUE(cp.tags.has_value());
    ASSERT_EQ(cp.tags->size(), 1u);
    EXPECT_EQ((*cp.tags)[0], "2nd");
    EXPECT_FALSE(cp.attributes.count("tag_0"));
    ASSERT_TRUE(cp.modified_by.has_value());
    EXPECT_EQ(*cp.modified_by, "slac-calendar-reader");
    EXPECT_EQ(cp.attributes.at("accel"), "lcls");
    EXPECT_EQ(cp.attributes.at("calendar"), "NC-CXI");
    // Per-shift fields live on the activation, not the configuration
    EXPECT_FALSE(cp.attributes.count("note"));
    EXPECT_FALSE(cp.attributes.count("poc"));
    EXPECT_FALSE(cp.attributes.count("config"));
    EXPECT_FALSE(cp.attributes.count("details"));
    EXPECT_EQ(cp.attributes.at("machine"), "NC");
    EXPECT_EQ(cp.attributes.at("hutch_name"), "CXI");
    EXPECT_EQ(cp.attributes.at("hutch_color"), "#a00000");
    EXPECT_EQ(cp.attributes.at("hutch_line"), "HXR");
    EXPECT_FALSE(cp.attributes.count("text_color"));

    // Second batch: ConfigurationActivationPayload
    ASSERT_TRUE(std::holds_alternative<ConfigurationActivationPayload>(batches[1].payload));
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    EXPECT_EQ(act.configuration_name, "CXI 1013443 Bain");
    ASSERT_TRUE(act.client_activation_id.has_value());
    EXPECT_EQ(*act.client_activation_id,
              "https://www.google.com/calendar/event?eid=abc123"
              "|2026-05-28T06:00:00-07:00|2026-05-28T18:00:00-07:00");
    EXPECT_EQ(act.start_time.epoch_seconds, static_cast<uint64_t>(1779973200));
    EXPECT_EQ(act.attributes.at("accel"), "lcls");
    EXPECT_EQ(act.attributes.at("calendar"), "NC-CXI");
    EXPECT_EQ(act.attributes.at("note"), "13.213 GeV, 80 pC");
    EXPECT_EQ(act.attributes.at("poc"), "Minitti");
    EXPECT_EQ(act.attributes.at("config"), "15 keV");
    EXPECT_EQ(act.attributes.at("hutch_name"), "CXI");
    EXPECT_EQ(act.attributes.at("hutch_line"), "HXR");
    EXPECT_FALSE(act.attributes.count("hutch_color"));
    EXPECT_FALSE(act.attributes.count("machine"));
    // details should be inner-text of the anchor
    EXPECT_EQ(act.attributes.at("details"), "https://pswww.slac.stanford.edu/foo");
}

TEST_F(SlacCalendarReaderTest, AttributeMappingOverridesRenameAndRetarget)
{
    server_.setResponse("lcls", kLclsEvent);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n"
        "  - field: poc\n"            // rename, keep default target (activation)
        "    name: person_on_shift\n"
        "  - field: note\n"           // retarget to both payloads
        "    target: both\n"
        "  - field: hutch.color\n"    // drop entirely
        "    target: none\n"
        "  - field: hutch.text_color\n" // new field, explicit name/target
        "    name: hutch_text_color\n"
        "    target: configuration\n");
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);

    EXPECT_FALSE(act.attributes.count("poc"));
    EXPECT_EQ(act.attributes.at("person_on_shift"), "Minitti");
    EXPECT_FALSE(cp.attributes.count("person_on_shift"));

    EXPECT_EQ(cp.attributes.at("note"), "13.213 GeV, 80 pC");
    EXPECT_EQ(act.attributes.at("note"), "13.213 GeV, 80 pC");

    EXPECT_FALSE(cp.attributes.count("hutch_color"));
    EXPECT_EQ(cp.attributes.at("hutch_text_color"), "white");
    EXPECT_FALSE(act.attributes.count("hutch_text_color"));

    // Untouched defaults still apply
    EXPECT_EQ(act.attributes.at("config"), "15 keV");
    EXPECT_EQ(cp.attributes.at("machine"), "NC");
}

TEST_F(SlacCalendarReaderTest, AttributeMappingRejectsBadTarget)
{
    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n  - field: poc\n    target: everywhere\n");
    using mldp_pvxs_driver::reader::impl::slac_calendar::SlacCalendarReaderConfig;
    EXPECT_THROW(SlacCalendarReaderConfig{cfg}, SlacCalendarReaderConfig::Error);
}

TEST_F(SlacCalendarReaderTest, AttributeMappingDefaults)
{
    using mldp_pvxs_driver::reader::impl::slac_calendar::SlacCalendarReaderConfig;
    using T = SlacCalendarReaderConfig::AttributeTarget;
    const SlacCalendarReaderConfig cfg{
        makeConfigFromYaml(makeReaderYaml(server_.baseUrl(), "  - lcls\n"))};

    const std::vector<std::tuple<std::string, std::string, T>> expected{
        {"calendar", "calendar", T::Both},
        {"machine", "machine", T::Configuration},
        {"hutch.name", "hutch_name", T::Both},
        {"hutch.line", "hutch_line", T::Both},
        {"hutch.color", "hutch_color", T::Configuration},
        {"note", "note", T::Activation},
        {"config", "config", T::Activation},
        {"poc", "poc", T::Activation},
        {"details", "details", T::Activation},
    };
    const auto& m = cfg.attributeMappings();
    ASSERT_EQ(m.size(), expected.size());
    for (size_t i = 0; i < m.size(); ++i)
    {
        EXPECT_EQ(m[i].field, std::get<0>(expected[i]));
        EXPECT_EQ(m[i].name, std::get<1>(expected[i]));
        EXPECT_EQ(m[i].target, std::get<2>(expected[i])) << m[i].field;
    }
}

TEST_F(SlacCalendarReaderTest, AttributeMappingParsesAllTargets)
{
    using mldp_pvxs_driver::reader::impl::slac_calendar::SlacCalendarReaderConfig;
    using T = SlacCalendarReaderConfig::AttributeTarget;
    const SlacCalendarReaderConfig cfg{makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n"
        "  - field: note\n    target: configuration\n"
        "  - field: poc\n    target: both\n"
        "  - field: machine\n    target: activation\n"
        "  - field: calendar\n    target: none\n")};

    const auto find = [&](const std::string& f) {
        for (const auto& m : cfg.attributeMappings())
            if (m.field == f)
                return m;
        ADD_FAILURE() << "missing mapping for " << f;
        return SlacCalendarReaderConfig::AttributeMapping{};
    };
    EXPECT_EQ(find("note").target, T::Configuration);
    EXPECT_EQ(find("poc").target, T::Both);
    EXPECT_EQ(find("machine").target, T::Activation);
    EXPECT_EQ(find("calendar").target, T::None);
    // overriding a default replaces it in place, no duplicate appended
    EXPECT_EQ(cfg.attributeMappings().size(), 9u);
}

TEST_F(SlacCalendarReaderTest, AttributeMappingRejectsInvalidEntries)
{
    using mldp_pvxs_driver::reader::impl::slac_calendar::SlacCalendarReaderConfig;
    const std::vector<std::pair<std::string, std::string>> bad{
        {"missing field", "attributes:\n  - name: foo\n"},
        {"empty field", "attributes:\n  - field: \"\"\n"},
        {"empty name", "attributes:\n  - field: poc\n    name: \"\"\n"},
        {"accel not remappable", "attributes:\n  - field: accel\n    target: none\n"},
        {"unknown target", "attributes:\n  - field: poc\n    target: Both\n"},
    };
    for (const auto& [label, yaml] : bad)
    {
        const auto cfg = makeConfigFromYaml(makeReaderYaml(server_.baseUrl(), "  - lcls\n") + yaml);
        EXPECT_THROW(SlacCalendarReaderConfig{cfg}, SlacCalendarReaderConfig::Error) << label;
    }
}

TEST_F(SlacCalendarReaderTest, AttributeMappingNewFieldDefaultsToFieldNameOnBoth)
{
    server_.setResponse("lcls", kLclsEvent);
    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n  - field: hutch.text_color\n");
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    EXPECT_EQ(cp.attributes.at("hutch.text_color"), "white");
    EXPECT_EQ(act.attributes.at("hutch.text_color"), "white");
}

TEST_F(SlacCalendarReaderTest, AttributeMappingRenamePreservesValueProcessing)
{
    // details keeps HTML stripping and calendar keeps whitespace normalization
    // regardless of the configured attribute name/target.
    server_.setResponse("lcls", R"json([{
        "url": "https://www.google.com/calendar/event?eid=p1",
        "program_name": "XPP Run",
        "calendar": "  NC   XPP ",
        "details": "<b>Actual start</b>: 18:06",
        "start": "2026-05-28T06:00:00-07:00",
        "end":   "2026-05-28T18:00:00-07:00"
    }])json");
    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n"
        "  - field: details\n    name: shift_details\n    target: both\n"
        "  - field: calendar\n    name: source_calendar\n    target: configuration\n");
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    EXPECT_EQ(cp.attributes.at("shift_details"), "Actual start: 18:06");
    EXPECT_EQ(act.attributes.at("shift_details"), "Actual start: 18:06");
    EXPECT_EQ(cp.attributes.at("source_calendar"), "NC XPP");
    EXPECT_FALSE(act.attributes.count("source_calendar"));
    EXPECT_FALSE(cp.attributes.count("calendar"));
    EXPECT_FALSE(act.attributes.count("calendar"));
}

TEST_F(SlacCalendarReaderTest, AttributeMappingAllNoneLeavesOnlyAccel)
{
    server_.setResponse("lcls", kLclsEvent);
    std::string yaml = makeReaderYaml(server_.baseUrl(), "  - lcls\n") + "attributes:\n";
    for (const char* f : {"calendar", "machine", "hutch.name", "hutch.line", "hutch.color",
                          "note", "config", "poc", "details"})
        yaml += std::string("  - field: ") + f + "\n    target: none\n";
    SlacCalendarReader reader(bus_, nullptr, makeConfigFromYaml(yaml));

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    ASSERT_EQ(cp.attributes.size(), 1u);
    EXPECT_EQ(cp.attributes.at("accel"), "lcls");
    ASSERT_EQ(act.attributes.size(), 1u);
    EXPECT_EQ(act.attributes.at("accel"), "lcls");
    // category disambiguation still uses the calendar value even when not emitted
    EXPECT_EQ(cp.configuration_name, "CXI 1013443 Bain");
}

TEST_F(SlacCalendarReaderTest, AttributeMappingSkipsMissingAndNonStringValues)
{
    server_.setResponse("lcls", kLclsEvent);
    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n"
        "  - field: does_not_exist\n"     // absent key
        "  - field: hutch.missing\n"      // absent nested key
        "  - field: machine.sub\n"        // parent is a string, not an object
        "  - field: a.b.c\n"              // absent multi-level path
        "  - field: tags\n"               // array value
        "  - field: hutch\n");            // object value
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    for (const char* k : {"does_not_exist", "hutch.missing", "machine.sub", "a.b.c", "tags", "hutch"})
    {
        EXPECT_FALSE(cp.attributes.count(k)) << k;
        EXPECT_FALSE(act.attributes.count(k)) << k;
    }
    // defaults unaffected
    EXPECT_EQ(cp.attributes.at("hutch_name"), "CXI");
    EXPECT_EQ(act.attributes.at("poc"), "Minitti");
}

TEST_F(SlacCalendarReaderTest, AttributeMappingDuplicateEntryLastWins)
{
    server_.setResponse("lcls", kLclsEvent);
    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n"
        "  - field: poc\n    name: first_name\n    target: configuration\n"
        "  - field: poc\n    name: second_name\n");   // keeps target from previous entry
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    EXPECT_EQ(cp.attributes.at("second_name"), "Minitti");
    EXPECT_FALSE(cp.attributes.count("first_name"));
    EXPECT_FALSE(act.attributes.count("second_name"));
    EXPECT_FALSE(act.attributes.count("poc"));
}

// Mirrors "Full Example with Attribute Customization" in
// docs/readers/slac-calendar-reader.md; keep both in sync.
TEST_F(SlacCalendarReaderTest, AttributeMappingDocumentedFullExample)
{
    server_.setResponse("lcls", kLclsEvent);
    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n") +
        "attributes:\n"
        "  - field: poc\n    name: person_on_shift\n"
        "  - field: note\n    name: electron_energy\n    target: both\n"
        "  - field: config\n    name: photon_energy\n    target: both\n"
        "  - field: machine\n    target: both\n"
        "  - field: hutch.color\n    target: none\n"
        "  - field: hutch.text_color\n    name: hutch_text_color\n    target: configuration\n");
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    const auto& cp  = std::get<ConfigurationPayload>(batches[0].payload);
    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);

    const std::map<std::string, std::string> expected_cfg{
        {"accel", "lcls"}, {"calendar", "NC-CXI"}, {"machine", "NC"},
        {"hutch_name", "CXI"}, {"hutch_line", "HXR"}, {"hutch_text_color", "white"},
        {"electron_energy", "13.213 GeV, 80 pC"}, {"photon_energy", "15 keV"}};
    const std::map<std::string, std::string> expected_act{
        {"accel", "lcls"}, {"calendar", "NC-CXI"}, {"machine", "NC"},
        {"hutch_name", "CXI"}, {"hutch_line", "HXR"},
        {"electron_energy", "13.213 GeV, 80 pC"}, {"photon_energy", "15 keV"},
        {"person_on_shift", "Minitti"}, {"details", "https://pswww.slac.stanford.edu/foo"}};

    using AttrMap = std::map<std::string, std::string>;
    EXPECT_EQ(AttrMap(cp.attributes.begin(), cp.attributes.end()), expected_cfg);
    EXPECT_EQ(AttrMap(act.attributes.begin(), act.attributes.end()), expected_act);
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
    EXPECT_EQ(cp.category, "Single bunch matching S20");
    EXPECT_FALSE(cp.description.has_value());
    EXPECT_FALSE(cp.tags.has_value());
    EXPECT_FALSE(cp.attributes.count("note"));
    EXPECT_FALSE(cp.attributes.count("poc"));
    EXPECT_FALSE(cp.attributes.count("config"));
    EXPECT_FALSE(cp.attributes.count("machine"));
    EXPECT_FALSE(cp.attributes.count("hutch_name"));
    EXPECT_EQ(cp.attributes.at("accel"), "facet");
    EXPECT_EQ(cp.attributes.at("calendar"), "FACET-MD");
}

TEST_F(SlacCalendarReaderTest, MultipleAccelsAllFetched)
{
    server_.setResponse("lcls", kLclsEvent);
    server_.setResponse("facet", kFacetEvent);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n  - facet\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    // 2 payloads per event × 2 accels = 4 total
    ASSERT_TRUE(bus_->waitForCount(4, std::chrono::milliseconds(5000)));

    ASSERT_TRUE(server_.waitForRequestCount(2, std::chrono::milliseconds(3000)));
    const auto history = server_.requestHistory();
    ASSERT_EQ(history.size(), 2u);

    bool found_lcls  = false;
    bool found_facet = false;
    for (const auto& h : history)
    {
        if (h.accel == "lcls")  found_lcls  = true;
        if (h.accel == "facet") found_facet = true;
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

TEST_F(SlacCalendarReaderTest, ConfigCategoryOverridesApiCalendar)
{
    server_.setResponse("lcls", kLclsEvent);

    const auto cfg = makeConfigFromYaml(
        makeReaderYamlWithCategory(server_.baseUrl(), "  - lcls\n", "MY-FIXED"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    ASSERT_GE(batches.size(), 1u);

    const auto& cp = std::get<ConfigurationPayload>(batches[0].payload);
    EXPECT_EQ(cp.category, "MY-FIXED");
    EXPECT_EQ(cp.attributes.at("calendar"), "NC-CXI");
}

TEST_F(SlacCalendarReaderTest, MissingCalendarFieldStillPushesConfig)
{
    static const char* kNoCalendar = R"json([
      {
        "url": "https://example.com/evt1",
        "program_name": "Test Program",
        "start": "2026-05-28T06:00:00-07:00",
        "end": "2026-05-28T18:00:00-07:00"
      }
    ])json";
    server_.setResponse("lcls", kNoCalendar);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    ASSERT_GE(batches.size(), 1u);

    const auto& cp = std::get<ConfigurationPayload>(batches[0].payload);
    EXPECT_EQ(cp.configuration_name, "Test Program");
    EXPECT_FALSE(cp.attributes.count("calendar"));
}

TEST_F(SlacCalendarReaderTest, MissingUrlFieldStillPushesActivation)
{
    static const char* kNoUrl = R"json([
      {
        "program_name": "Test Program",
        "calendar": "NC-TEST",
        "start": "2026-05-28T06:00:00-07:00",
        "end": "2026-05-28T18:00:00-07:00"
      }
    ])json";
    server_.setResponse("lcls", kNoUrl);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(2, std::chrono::milliseconds(5000)));
    const auto batches = bus_->snapshot();
    ASSERT_EQ(batches.size(), 2u);

    const auto& act = std::get<ConfigurationActivationPayload>(batches[1].payload);
    EXPECT_EQ(act.configuration_name, "Test Program");
    EXPECT_FALSE(act.client_activation_id.has_value());
}

TEST_F(SlacCalendarReaderTest, MissingStartStillPushesConfig)
{
    static const char* kNoStart = R"json([
      {
        "url": "https://example.com/evt1",
        "program_name": "Test Program",
        "calendar": "NC-TEST",
        "end": "2026-05-28T18:00:00-07:00"
      }
    ])json";
    server_.setResponse("lcls", kNoStart);

    const auto cfg = makeConfigFromYaml(
        makeReaderYaml(server_.baseUrl(), "  - lcls\n"));
    SlacCalendarReader reader(bus_, nullptr, cfg);

    ASSERT_TRUE(bus_->waitForCount(1, std::chrono::milliseconds(5000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto batches = bus_->snapshot();
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ConfigurationPayload>(batches[0].payload));
}
