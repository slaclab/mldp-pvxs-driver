//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_ds/EpicsDSMetadataReaderConfig.h>

namespace mldp_pvxs_driver::reader::impl::epics_ds {

static constexpr auto kNameKey              = "name";
static constexpr auto kServiceKey           = "service";
static constexpr auto kQueryKey             = "query";
static constexpr auto kTimeoutSecKey        = "timeout-sec";
static constexpr auto kSourceNameColumnKey  = "source-name-column";
static constexpr auto kTagsColumnKey        = "tags-column";
static constexpr auto kShowColumnsKey       = "show-columns";
static constexpr auto kRescanIntervalSecKey = "rescan-interval-sec";

EpicsDSMetadataReaderConfig::EpicsDSMetadataReaderConfig(const config::Config& cfg)
{
    parse(cfg);
}

void EpicsDSMetadataReaderConfig::parse(const config::Config& cfg)
{
    if (!cfg.hasChild(kNameKey))
        throw Error("epics-ds-metadata reader: 'name' is required");
    name_ = cfg.get(kNameKey);
    if (name_.empty())
        throw Error("epics-ds-metadata reader: 'name' must not be empty");

    service_             = cfg.get(kServiceKey, "ds");
    query_               = cfg.get(kQueryKey, "%");
    timeout_sec_         = cfg.getDouble(kTimeoutSecKey, 5.0);
    source_name_column_  = cfg.get(kSourceNameColumnKey, "channelName");
    tags_column_         = cfg.get(kTagsColumnKey, "");
    show_columns_        = cfg.get(kShowColumnsKey, "");
    rescan_interval_sec_ = cfg.getDouble(kRescanIntervalSecKey, 0.0);

    if (timeout_sec_ <= 0.0)
        throw Error("epics-ds-metadata reader: 'timeout-sec' must be positive");
    if (rescan_interval_sec_ < 0.0)
        throw Error("epics-ds-metadata reader: 'rescan-interval-sec' must be >= 0");

    valid_ = true;
}

} // namespace mldp_pvxs_driver::reader::impl::epics_ds
