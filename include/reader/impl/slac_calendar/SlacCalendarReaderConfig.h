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

#include <config/Config.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::slac_calendar {

class SlacCalendarReaderConfig
{
public:
    struct Error : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    explicit SlacCalendarReaderConfig(const config::Config& cfg);

    bool valid() const noexcept { return valid_; }

    const std::string& name() const noexcept { return name_; }
    const std::string& baseUrl() const noexcept { return base_url_; }
    const std::vector<std::string>& experiments() const noexcept { return experiments_; }
    int lookaheadDays() const noexcept { return lookahead_days_; }
    int lookbackDays() const noexcept { return lookback_days_; }
    const std::optional<std::string>& startDate() const noexcept { return start_date_; }
    const std::optional<std::string>& endDate() const noexcept { return end_date_; }
    const std::optional<std::string>& category() const noexcept { return category_; }
    double rescanIntervalSec() const noexcept { return rescan_interval_sec_; }
    long connectTimeoutSec() const noexcept { return connect_timeout_sec_; }
    long totalTimeoutSec() const noexcept { return total_timeout_sec_; }
    bool tlsVerifyPeer() const noexcept { return tls_verify_peer_; }
    bool tlsVerifyHost() const noexcept { return tls_verify_host_; }
    int eventLimit() const noexcept { return event_limit_; }

private:
    void parse(const config::Config& cfg);

    bool                     valid_{false};
    std::string              name_;
    std::string              base_url_;
    std::vector<std::string> experiments_;
    int                      lookahead_days_{30};
    int                      lookback_days_{1};
    std::optional<std::string> start_date_;
    std::optional<std::string> end_date_;
    std::optional<std::string> category_;
    double                   rescan_interval_sec_{0.0};
    long                     connect_timeout_sec_{30};
    long                     total_timeout_sec_{60};
    bool                     tls_verify_peer_{true};
    bool                     tls_verify_host_{true};
    int                      event_limit_{1000};
};

} // namespace mldp_pvxs_driver::reader::impl::slac_calendar
