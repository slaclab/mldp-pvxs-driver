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
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

struct PvEntry {
    std::string name;
    std::string option_type;   // "none", "scalar", "slac-bsas-table"
    std::string option_value;  // for scalar
    std::string ts_seconds;    // for slac-bsas-table
    std::string ts_nanos;      // for slac-bsas-table
};

struct MldpWriterConfig {
    std::string name;
    std::string thread_pool        = "1";
    std::string stream_max_bytes   = "2097152";
    std::string stream_max_age_ms  = "200";
    std::string provider_name;
    std::string provider_desc;
    std::string ingestion_url;
    std::string query_url;
    std::string min_conn           = "1";
    std::string max_conn           = "4";
    std::string creds_type         = "ssl"; // "none","ssl","custom-tls"
    std::string pem_cert_chain;
    std::string pem_private_key;
    std::string pem_root_certs;
};

struct Hdf5WriterConfig {
    std::string name;
    std::string base_path;
    std::string max_file_age_s     = "3600";
    std::string max_file_size_mb   = "512";
    std::string flush_interval_ms  = "1000";
    std::string compression_level  = "0";
    bool        is_merge           = false;
};

struct EpicsReaderConfig {
    std::string name;
    std::string reader_type;       // "epics-pvxs","epics-base","epics-archiver"
    std::string thread_pool        = "2";
    std::string column_batch_size  = "50";
    // epics-base only
    std::string monitor_poll_threads       = "2";
    std::string monitor_poll_interval_ms   = "5";
    // epics-archiver only
    std::string hostname;
    std::string mode               = "historical_once";
    std::string start_date;
    std::string end_date;
    std::string poll_interval_sec;
    std::string lookback_sec;
    std::string connect_timeout_sec = "30";
    std::string total_timeout_sec   = "300";
    std::string batch_duration_sec  = "1";
    std::string tls_verify_peer     = "true";
    std::string tls_verify_host     = "true";
    std::vector<PvEntry> pvs;
};

struct RoutingEntry {
    std::string writer_name;
    std::vector<std::string> from_readers;   // empty = all
    std::vector<std::string> include_globs;
    std::vector<std::string> exclude_globs;
};

struct WizardState {
    // Phase 1
    std::string controller_name = "default";
    // Phase 2
    std::vector<MldpWriterConfig> mldp_writers;
    std::vector<Hdf5WriterConfig> hdf5_writers;
    // Phase 3
    std::vector<EpicsReaderConfig> readers;
    // Phase 4
    bool        metrics_enabled   = false;
    std::string metrics_endpoint  = "0.0.0.0:9464";
    std::string metrics_interval  = "1";
    // Phase 5
    bool routing_all_to_all = true;
    std::vector<RoutingEntry> routing;
};

int runWizard(const std::string& output_path, const std::string& from_path);

} // namespace mldp_pvxs_driver::config
