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

#include <reader/impl/slac_calendar/SlacCalendarReaderConfig.h>

#include "../../../config/test_config_helpers.h"

using mldp_pvxs_driver::config::makeConfigFromYaml;
using mldp_pvxs_driver::reader::impl::slac_calendar::SlacCalendarReaderConfig;

namespace {

void assertNoThrow(const mldp_pvxs_driver::config::Config& cfg)
{
    SlacCalendarReaderConfig c(cfg);
    (void)c;
}

void assertThrows(const mldp_pvxs_driver::config::Config& cfg)
{
    EXPECT_THROW(assertNoThrow(cfg), SlacCalendarReaderConfig::Error);
}

} // namespace

static std::string minimalYaml()
{
    return R"yaml(
name: test-reader
base-url: http://localhost:8080
experiments:
  - lcls
  - facet
lookahead-days: 30
)yaml";
}

TEST(SlacCalendarReaderConfigTest, ParsesMinimalConfig)
{
    const auto cfg = makeConfigFromYaml(minimalYaml());
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    EXPECT_TRUE(c.valid());
    EXPECT_EQ(c.name(), "test-reader");
    EXPECT_EQ(c.baseUrl(), "http://localhost:8080");
    ASSERT_EQ(c.experiments().size(), 2u);
    EXPECT_EQ(c.experiments()[0], "lcls");
    EXPECT_EQ(c.experiments()[1], "facet");
    EXPECT_EQ(c.lookaheadDays(), 30);
    EXPECT_EQ(c.lookbackDays(), 1);
    EXPECT_FALSE(c.startDate().has_value());
    EXPECT_DOUBLE_EQ(c.rescanIntervalSec(), 0.0);
    EXPECT_EQ(c.connectTimeoutSec(), 30L);
    EXPECT_EQ(c.totalTimeoutSec(), 60L);
    EXPECT_TRUE(c.tlsVerifyPeer());
    EXPECT_TRUE(c.tlsVerifyHost());
    EXPECT_EQ(c.eventLimit(), 1000);
}

TEST(SlacCalendarReaderConfigTest, ParsesFullConfig)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: full-reader
base-url: https://api.slac.stanford.edu/calendar
experiments:
  - lcls
lookahead-days: 14
lookback-days: 2
start-date: "2026-01-01"
rescan-interval-sec: 300.0
connect-timeout-sec: 10
total-timeout-sec: 120
tls-verify-peer: false
tls-verify-host: false
event-limit: 500
)yaml");

    SlacCalendarReaderConfig c(cfg);
    EXPECT_TRUE(c.valid());
    EXPECT_EQ(c.lookaheadDays(), 14);
    EXPECT_EQ(c.lookbackDays(), 2);
    ASSERT_TRUE(c.startDate().has_value());
    EXPECT_EQ(*c.startDate(), "2026-01-01");
    EXPECT_DOUBLE_EQ(c.rescanIntervalSec(), 300.0);
    EXPECT_EQ(c.connectTimeoutSec(), 10L);
    EXPECT_EQ(c.totalTimeoutSec(), 120L);
    EXPECT_FALSE(c.tlsVerifyPeer());
    EXPECT_FALSE(c.tlsVerifyHost());
    EXPECT_EQ(c.eventLimit(), 500);
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenNameMissing)
{
    assertThrows(makeConfigFromYaml(R"yaml(
base-url: http://localhost
experiments:
  - lcls
lookahead-days: 7
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenBaseUrlMissing)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
experiments:
  - lcls
lookahead-days: 7
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenExperimentsMissing)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
lookahead-days: 7
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenLookaheadDaysMissing)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenLookaheadDaysZero)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
lookahead-days: 0
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenLookbackDaysNegative)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
lookahead-days: 7
lookback-days: -1
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenTotalTimeoutLessThanConnect)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
lookahead-days: 7
connect-timeout-sec: 60
total-timeout-sec: 30
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenStartDateBadFormat)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
lookahead-days: 7
start-date: "01/01/2026"
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ParsesStartDateWithTime)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
lookahead-days: 7
start-date: "2026-01-01T08:30:00"
)yaml");
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    ASSERT_TRUE(c.startDate().has_value());
    EXPECT_EQ(*c.startDate(), "2026-01-01T08:30:00");
}

TEST(SlacCalendarReaderConfigTest, ParsesEndDateDateOnly)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2026-01-01"
end-date: "2026-02-01"
)yaml");
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    ASSERT_TRUE(c.endDate().has_value());
    EXPECT_EQ(*c.endDate(), "2026-02-01");
}

TEST(SlacCalendarReaderConfigTest, ParsesEndDateWithTime)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2026-01-01T00:00:00"
end-date: "2026-02-01T23:59:59"
)yaml");
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    ASSERT_TRUE(c.endDate().has_value());
    EXPECT_EQ(*c.endDate(), "2026-02-01T23:59:59");
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenEndDateWithoutStartDate)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
end-date: "2026-02-01"
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenEndDateBeforeStartDate)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2026-03-01"
end-date: "2026-01-01"
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenEndDateWithLookaheadDays)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2026-01-01"
end-date: "2026-02-01"
lookahead-days: 7
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ThrowsWhenEndDateWithRescanInterval)
{
    assertThrows(makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2026-01-01"
end-date: "2026-02-01"
rescan-interval-sec: 300
)yaml"));
}

TEST(SlacCalendarReaderConfigTest, ParsesEndDateWithTimezone)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2025-01-01T00:00:00Z"
end-date: "2025-12-31T23:59:59Z"
)yaml");
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    ASSERT_TRUE(c.startDate().has_value());
    ASSERT_TRUE(c.endDate().has_value());
    EXPECT_EQ(*c.startDate(), "2025-01-01T00:00:00Z");
    EXPECT_EQ(*c.endDate(), "2025-12-31T23:59:59Z");
}

TEST(SlacCalendarReaderConfigTest, ParsesEndDateWithPosixTimezone)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2025-01-01T00:00:00-08:00"
end-date: "2025-12-31T23:59:59-08:00"
)yaml");
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    EXPECT_TRUE(c.startDate().has_value());
    EXPECT_TRUE(c.endDate().has_value());
}

TEST(SlacCalendarReaderConfigTest, LookaheadDaysNotRequiredWhenEndDateSet)
{
    const auto cfg = makeConfigFromYaml(R"yaml(
name: r
base-url: http://localhost
experiments:
  - lcls
start-date: "2026-01-01"
end-date: "2026-02-01"
)yaml");
    ASSERT_NO_THROW(assertNoThrow(cfg));
    SlacCalendarReaderConfig c(cfg);
    EXPECT_TRUE(c.valid());
}
