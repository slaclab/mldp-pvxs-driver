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

struct EditListOptions {
    std::string path = "config.yaml";
};

struct EditRemoveOptions {
    std::string path       = "config.yaml";
    std::string kind;       // "writer", "reader", "routing"
    std::string name;
    bool        no_backup  = false;
    bool        dry_run    = false;
};

struct EditAddOptions {
    std::string path       = "config.yaml";
    std::string kind;       // "writer", "reader", "routing"
    // common
    std::string name;
    std::string type;       // writer: mldp|hdf5|hdf5-merge  reader: epics-pvxs|epics-base|epics-archiver
    // mldp writer
    std::string thread_pool;
    std::string ingestion_url;
    std::string provider_name;
    std::string query_url;
    std::string min_conn;
    std::string max_conn;
    std::string credentials;
    std::string stream_max_bytes;
    std::string stream_max_age_ms;
    // hdf5 writer
    std::string base_path;
    std::string compression_level;
    std::string max_file_age_s;
    std::string max_file_size_mb;
    // reader
    std::string reader_thread_pool;
    std::string column_batch_size;
    std::string pvs;               // comma-separated PV names
    std::string hostname;
    std::string mode;
    std::string start_date;
    std::string end_date;
    std::string poll_interval_sec;
    std::string connect_timeout_sec;
    std::string total_timeout_sec;
    // routing
    std::string writer_name;
    std::string from;              // comma-separated reader names or "all"
    std::vector<std::string> include_globs;
    std::vector<std::string> exclude_globs;
    bool replace  = false;
    bool no_backup = false;
    bool dry_run  = false;
};

int runList(const EditListOptions& opts);
int runRemove(const EditRemoveOptions& opts);
int runAdd(const EditAddOptions& opts);

#ifdef MLDP_WIZARD_ENABLED
// Interactive add path — launches FTXUI sub-flow for the given kind.
int runAddInteractive(const std::string& path, const std::string& kind,
                      bool no_backup, bool dry_run);
#endif

} // namespace mldp_pvxs_driver::config
