//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "wizard_internal.h"
#include <config/Config.h>
#include <config/wizard.h>

#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Pure (no FTXUI) functions — compiled unconditionally so tests can link them
// ─────────────────────────────────────────────────────────────────────────────

namespace mldp_pvxs_driver::config::wizard_internal {

bool isValidIso8601(const std::string& s)
{
    // Accept YYYY-MM-DDTHH:MM:SSZ / +HH:MM / -HH:MM / bare YYYY-MM-DD
    static const std::regex re(
        R"(^\d{4}-\d{2}-\d{2}(T\d{2}:\d{2}:\d{2}(Z|[+-]\d{2}:\d{2})?)?$)");
    return std::regex_match(s, re);
}

bool isPositiveInt(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return std::stoi(s) > 0;
}

bool isNonNegInt(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

bool isPositiveDouble(const std::string& s)
{
    try
    {
        return std::stod(s) > 0.0;
    }
    catch (...)
    {
        return false;
    }
}

bool isNonNegDouble(const std::string& s)
{
    try
    {
        return std::stod(s) >= 0.0;
    }
    catch (...)
    {
        return false;
    }
}

static std::string ind(int n)
{
    return std::string(static_cast<std::size_t>(n * 2), ' ');
}

std::string generateYaml(const WizardState& st)
{
    std::ostringstream o;

    // controller
    o << "controller:\n";
    o << ind(1) << "name: " << (st.controller_name.empty() ? "default" : st.controller_name) << "\n\n";

    // writers
    o << "writer:\n";
    if (!st.mldp_writers.empty())
    {
        o << ind(1) << "mldp:\n";
        for (const auto& w : st.mldp_writers)
        {
            o << ind(2) << "- name: " << w.name << "\n";
            o << ind(3) << "thread-pool: " << w.thread_pool << "\n";
            o << ind(3) << "stream-max-bytes: " << w.stream_max_bytes << "\n";
            o << ind(3) << "stream-max-age-ms: " << w.stream_max_age_ms << "\n";
            o << ind(3) << "mldp-pool:\n";
            o << ind(4) << "provider-name: " << w.provider_name << "\n";
            if (!w.provider_desc.empty())
                o << ind(4) << "provider-description: \"" << w.provider_desc << "\"\n";
            o << ind(4) << "ingestion-url: " << w.ingestion_url << "\n";
            if (!w.query_url.empty())
                o << ind(4) << "query-url: " << w.query_url << "\n";
            o << ind(4) << "min-conn: " << w.min_conn << "\n";
            o << ind(4) << "max-conn: " << w.max_conn << "\n";
            if (w.creds_type == "none")
            {
                o << ind(4) << "credentials: none\n";
            }
            else if (w.creds_type == "ssl")
            {
                o << ind(4) << "credentials: ssl\n";
            }
            else
            {
                // custom-tls
                o << ind(4) << "credentials:\n";
                if (!w.pem_cert_chain.empty())
                    o << ind(5) << "pem-cert-chain: " << w.pem_cert_chain << "\n";
                if (!w.pem_private_key.empty())
                    o << ind(5) << "pem-private-key: " << w.pem_private_key << "\n";
                if (!w.pem_root_certs.empty())
                    o << ind(5) << "pem-root-certs: " << w.pem_root_certs << "\n";
            }
        }
    }
    if (!st.hdf5_writers.empty())
    {
        // group by is_merge
        bool any_hdf5 = false;
        bool any_hdf5_merge = false;
        for (const auto& w : st.hdf5_writers)
        {
            if (w.is_merge)
                any_hdf5_merge = true;
            else
                any_hdf5 = true;
        }
        auto emitHdf5 = [&](bool merge)
        {
            o << ind(1) << (merge ? "hdf5-merge" : "hdf5") << ":\n";
            for (const auto& w : st.hdf5_writers)
            {
                if (w.is_merge != merge)
                    continue;
                o << ind(2) << "- name: " << w.name << "\n";
                o << ind(3) << "base-path: " << w.base_path << "\n";
                o << ind(3) << "max-file-age-s: " << w.max_file_age_s << "\n";
                o << ind(3) << "max-file-size-mb: " << w.max_file_size_mb << "\n";
                o << ind(3) << "flush-interval-ms: " << w.flush_interval_ms << "\n";
                o << ind(3) << "compression-level: " << w.compression_level << "\n";
            }
        };
        if (any_hdf5)
            emitHdf5(false);
        if (any_hdf5_merge)
            emitHdf5(true);
    }
    o << "\n";

    // readers — group by type, emit map-key format matching writer section
    if (!st.readers.empty())
    {
        o << "reader:\n";
        std::string lastType;
        for (const auto& r : st.readers)
        {
            if (r.reader_type != lastType)
            {
                o << ind(1) << r.reader_type << ":\n";
                lastType = r.reader_type;
            }
            o << ind(2) << "- name: " << r.name << "\n";
            if (r.reader_type == "epics-pvxs" || r.reader_type == "epics-base")
            {
                o << ind(3) << "thread-pool: " << r.thread_pool << "\n";
                o << ind(3) << "column-batch-size: " << r.column_batch_size << "\n";
                if (r.reader_type == "epics-base")
                {
                    o << ind(3) << "monitor-poll-threads: " << r.monitor_poll_threads << "\n";
                    o << ind(3) << "monitor-poll-interval-ms: " << r.monitor_poll_interval_ms << "\n";
                }
            }
            else if (r.reader_type == "epics-archiver")
            {
                o << ind(3) << "hostname: " << r.hostname << "\n";
                o << ind(3) << "mode: " << r.mode << "\n";
                if (r.mode == "historical_once")
                {
                    o << ind(3) << "start-date: \"" << r.start_date << "\"\n";
                    if (!r.end_date.empty())
                        o << ind(3) << "end-date: \"" << r.end_date << "\"\n";
                }
                else
                {
                    o << ind(3) << "poll-interval-sec: " << r.poll_interval_sec << "\n";
                    if (!r.lookback_sec.empty())
                        o << ind(3) << "lookback-sec: " << r.lookback_sec << "\n";
                }
                o << ind(3) << "connect-timeout-sec: " << r.connect_timeout_sec << "\n";
                o << ind(3) << "total-timeout-sec: " << r.total_timeout_sec << "\n";
                o << ind(3) << "tls-verify-peer: " << r.tls_verify_peer << "\n";
                o << ind(3) << "tls-verify-host: " << r.tls_verify_host << "\n";
            }
            else if (r.reader_type == "epics-ds-metadata")
            {
                o << ind(3) << "service: "             << r.ds_service             << "\n";
                o << ind(3) << "query: "               << r.ds_query               << "\n";
                o << ind(3) << "timeout-sec: "         << r.ds_timeout_sec         << "\n";
                o << ind(3) << "source-name-column: "  << r.ds_source_name_col     << "\n";
                if (!r.ds_tags_col.empty())
                    o << ind(3) << "tags-column: "     << r.ds_tags_col            << "\n";
                o << ind(3) << "rescan-interval-sec: " << r.ds_rescan_interval_sec << "\n";
            }
            if (!r.static_metadata.empty())
            {
                o << ind(3) << "static-metadata:\n";
                for (const auto& [k, v] : r.static_metadata)
                    o << ind(4) << k << ": " << v << "\n";
            }
            if (!r.pvs.empty())
            {
                o << ind(3) << "pvs:\n";
                for (const auto& pv : r.pvs)
                {
                    if (pv.option_type == "none" || pv.option_type.empty())
                    {
                        o << ind(4) << "- name: " << pv.name << "\n";
                    }
                    else if (pv.option_type == "scalar")
                    {
                        o << ind(4) << "- name: " << pv.name << "\n";
                        o << ind(5) << "option: \"" << pv.option_value << "\"\n";
                    }
                    else if (pv.option_type == "slac-bsas-table")
                    {
                        o << ind(4) << "- name: " << pv.name << "\n";
                        o << ind(5) << "option:\n";
                        o << ind(6) << "type: slac-bsas-table\n";
                        o << ind(6) << "tsSeconds: " << pv.ts_seconds << "\n";
                        o << ind(6) << "tsNanos: " << pv.ts_nanos << "\n";
                    }
                    if (!pv.metadata.empty())
                    {
                        o << ind(5) << "metadata:\n";
                        for (const auto& [k, v] : pv.metadata)
                            o << ind(6) << k << ": " << v << "\n";
                    }
                }
            }
        }
        o << "\n";
    }

    // metrics
    if (st.metrics_enabled)
    {
        o << "metrics:\n";
        o << ind(1) << "endpoint: \"" << st.metrics_endpoint << "\"\n";
        o << ind(1) << "scan-interval-seconds: " << st.metrics_interval << "\n\n";
    }

    // queryable
    if (st.queryable.mldp.enabled || st.queryable.mldp_pv_metadata.enabled)
    {
        o << "queryable:\n";
        if (st.queryable.mldp.enabled)
        {
            o << ind(1) << "mldp:\n";
            o << ind(2) << "mldp-pool:\n";
            o << ind(3) << "ingestion-url: " << st.queryable.mldp.ingestion_url << "\n";
            if (!st.queryable.mldp.query_url.empty())
                o << ind(3) << "query-url: " << st.queryable.mldp.query_url << "\n";
            o << ind(3) << "min-conn: " << st.queryable.mldp.min_conn << "\n";
            o << ind(3) << "max-conn: " << st.queryable.mldp.max_conn << "\n";
        }
        if (st.queryable.mldp_pv_metadata.enabled)
        {
            o << ind(1) << "mldp-pv-metadata:\n";
            o << ind(2) << "mldp-pv-metadata-pool:\n";
            o << ind(3) << "annotation-url: " << st.queryable.mldp_pv_metadata.annotation_url << "\n";
            o << ind(3) << "min-conn: " << st.queryable.mldp_pv_metadata.min_conn << "\n";
            o << ind(3) << "max-conn: " << st.queryable.mldp_pv_metadata.max_conn << "\n";
        }
        o << "\n";
    }

    // routing
    if (!st.routing_all_to_all && !st.routing.empty())
    {
        o << "routing:\n";
        for (const auto& re : st.routing)
        {
            o << ind(1) << re.writer_name << ":\n";
            if (!re.from_readers.empty())
            {
                o << ind(2) << "from:\n";
                for (const auto& r : re.from_readers)
                    o << ind(3) << "- " << r << "\n";
            }
            if (!re.include_globs.empty())
            {
                o << ind(2) << "include:\n";
                for (const auto& g : re.include_globs)
                    o << ind(3) << "- \"" << g << "\"\n";
            }
            if (!re.exclude_globs.empty())
            {
                o << ind(2) << "exclude:\n";
                for (const auto& g : re.exclude_globs)
                    o << ind(3) << "- \"" << g << "\"\n";
            }
        }
        o << "\n";
    }

    return o.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// --from amend: load existing YAML into WizardState
// ─────────────────────────────────────────────────────────────────────────────

void loadFromConfig(const std::string& path, WizardState& st)
{
    Config cfg;
    try
    {
        cfg = Config::configFromFile(path);
    }
    catch (...)
    {
        std::cerr << "Warning: could not parse '" << path << "' for amend mode — starting fresh.\n";
        return;
    }

    // controller
    if (cfg.hasChild("controller"))
    {
        auto cv = cfg.subConfig("controller");
        if (!cv.empty())
            st.controller_name = cv[0].get("name", "default");
    }

    // metrics
    if (cfg.hasChild("metrics"))
    {
        auto mv = cfg.subConfig("metrics");
        if (!mv.empty())
        {
            const auto& m = mv[0];
            st.metrics_enabled = true;
            st.metrics_endpoint = m.get("endpoint", "0.0.0.0:9464");
            st.metrics_interval = std::to_string(m.getInt("scan-interval-seconds", 1));
        }
    }

    // queryable
    if (cfg.hasChild("queryable"))
    {
        auto qv = cfg.subConfig("queryable");
        if (!qv.empty())
        {
            const auto& q = qv[0];
            if (q.hasChild("mldp"))
            {
                auto mv = q.subConfig("mldp");
                if (!mv.empty() && mv[0].hasChild("mldp-pool"))
                {
                    auto pv = mv[0].subConfig("mldp-pool");
                    if (!pv.empty())
                    {
                        const auto& p = pv[0];
                        st.queryable.mldp.enabled = true;
                        st.queryable.mldp.ingestion_url = p.get("ingestion-url", "");
                        st.queryable.mldp.query_url = p.get("query-url", "");
                        st.queryable.mldp.min_conn = std::to_string(p.getInt("min-conn", 1));
                        st.queryable.mldp.max_conn = std::to_string(p.getInt("max-conn", 2));
                    }
                }
            }
            if (q.hasChild("mldp-pv-metadata"))
            {
                auto mv = q.subConfig("mldp-pv-metadata");
                if (!mv.empty() && mv[0].hasChild("mldp-pv-metadata-pool"))
                {
                    auto pv = mv[0].subConfig("mldp-pv-metadata-pool");
                    if (!pv.empty())
                    {
                        const auto& p = pv[0];
                        st.queryable.mldp_pv_metadata.enabled = true;
                        st.queryable.mldp_pv_metadata.annotation_url = p.get("annotation-url", "");
                        st.queryable.mldp_pv_metadata.min_conn = std::to_string(p.getInt("min-conn", 1));
                        st.queryable.mldp_pv_metadata.max_conn = std::to_string(p.getInt("max-conn", 2));
                    }
                }
            }
        }
    }

    if (!cfg.hasChild("writer"))
        return;
    auto writerVec = cfg.subConfig("writer");
    if (writerVec.empty())
        return;
    const Config& writer = writerVec[0];

    // mldp writers
    for (const auto& inst : writer.subConfig("mldp"))
    {
        MldpWriterConfig w;
        w.name = inst.get("name", "");
        w.thread_pool = std::to_string(inst.getInt("thread-pool", 1));
        w.stream_max_bytes = std::to_string(inst.getInt("stream-max-bytes", 2097152));
        w.stream_max_age_ms = std::to_string(inst.getInt("stream-max-age-ms", 200));
        if (inst.hasChild("mldp-pool"))
        {
            auto pv = inst.subConfig("mldp-pool");
            if (!pv.empty())
            {
                const auto& p = pv[0];
                w.provider_name = p.get("provider-name", "");
                w.provider_desc = p.get("provider-description", "");
                w.ingestion_url = p.get("ingestion-url", "");
                w.query_url = p.get("query-url", "");
                w.min_conn = std::to_string(p.getInt("min-conn", 1));
                w.max_conn = std::to_string(p.getInt("max-conn", 4));
                std::string cred = p.get("credentials", "ssl");
                if (cred == "none" || cred == "ssl")
                {
                    w.creds_type = cred;
                }
                else
                {
                    w.creds_type = "custom-tls";
                }
            }
        }
        st.mldp_writers.push_back(std::move(w));
    }

    // hdf5 / hdf5-merge writers
    auto loadHdf5 = [&](const std::string& tag, bool is_merge)
    {
        for (const auto& inst : writer.subConfig(tag))
        {
            Hdf5WriterConfig w;
            w.is_merge = is_merge;
            w.name = inst.get("name", "");
            w.base_path = inst.get("base-path", "");
            w.max_file_age_s = std::to_string(inst.getInt("max-file-age-s", 3600));
            w.max_file_size_mb = std::to_string(inst.getInt("max-file-size-mb", 512));
            w.flush_interval_ms = std::to_string(inst.getInt("flush-interval-ms", 1000));
            w.compression_level = std::to_string(inst.getInt("compression-level", 0));
            st.hdf5_writers.push_back(std::move(w));
        }
    };
    loadHdf5("hdf5", false);
    loadHdf5("hdf5-merge", true);

    // readers
    if (!cfg.hasChild("reader"))
        return;
    for (const auto& rentry : cfg.subConfig("reader"))
    {
        static const std::vector<std::string> rtypes =
            {"epics-pvxs", "epics-base", "epics-archiver", "epics-ds-metadata"};
        for (const auto& rtype : rtypes)
        {
            if (!rentry.hasChild(rtype))
                continue;
            for (const auto& inst : rentry.subConfig(rtype))
            {
                EpicsReaderConfig r;
                r.reader_type = rtype;
                r.name = inst.get("name", "");
                r.thread_pool = std::to_string(inst.getInt("thread-pool", 2));
                r.column_batch_size = std::to_string(inst.getInt("column-batch-size", 50));
                if (rtype == "epics-base")
                {
                    r.monitor_poll_threads = std::to_string(inst.getInt("monitor-poll-threads", 2));
                    r.monitor_poll_interval_ms = std::to_string(inst.getInt("monitor-poll-interval-ms", 5));
                }
                if (rtype == "epics-archiver")
                {
                    r.hostname = inst.get("hostname", "");
                    r.mode = inst.get("mode", "historical_once");
                    r.start_date = inst.get("start-date", "");
                    r.end_date = inst.get("end-date", "");
                    r.poll_interval_sec = std::to_string(inst.getInt("poll-interval-sec", 0));
                    r.lookback_sec = std::to_string(inst.getInt("lookback-sec", 0));
                    r.connect_timeout_sec = std::to_string(inst.getInt("connect-timeout-sec", 30));
                    r.total_timeout_sec = std::to_string(inst.getInt("total-timeout-sec", 300));
                    r.tls_verify_peer = inst.getBool("tls-verify-peer", true) ? "true" : "false";
                    r.tls_verify_host = inst.getBool("tls-verify-host", true) ? "true" : "false";
                }
                if (rtype == "epics-ds-metadata")
                {
                    r.ds_service           = inst.get("service",            "ds");
                    r.ds_query             = inst.get("query",              "%");
                    r.ds_timeout_sec       = std::to_string(inst.getDouble("timeout-sec",         5.0));
                    r.ds_source_name_col   = inst.get("source-name-column", "channelName");
                    r.ds_tags_col          = inst.get("tags-column",        "");
                    r.ds_rescan_interval_sec = std::to_string(inst.getDouble("rescan-interval-sec", 0.0));
                }
                // reader-level static-metadata
                if (inst.hasChild("static-metadata"))
                {
                    auto mv = inst.subConfig("static-metadata");
                    if (!mv.empty())
                    {
                        const auto& raw = mv[0].raw();
                        if (!raw.invalid() && raw.is_map())
                        {
                            for (const auto child : raw.children())
                            {
                                if (child.has_key() && child.has_val())
                                {
                                    std::string k{child.key().str, child.key().len};
                                    std::string v;
                                    child >> v;
                                    r.static_metadata.emplace_back(k, v);
                                }
                            }
                        }
                    }
                }
                if (inst.hasChild("pvs"))
                {
                    for (const auto& pvNode : inst.subConfig("pvs"))
                    {
                        PvEntry pv;
                        pv.name = pvNode.get("name", "");
                        pv.option_type = "none";
                        if (pvNode.hasChild("option"))
                        {
                            auto optVec = pvNode.subConfig("option");
                            if (!optVec.empty() && optVec[0].raw().is_map() &&
                                optVec[0].hasChild("type"))
                            {
                                pv.option_type = optVec[0].get("type", "none");
                                pv.ts_seconds = optVec[0].get("tsSeconds", "");
                                pv.ts_nanos = optVec[0].get("tsNanos", "");
                            }
                            else
                            {
                                pv.option_type = "scalar";
                                pv.option_value = pvNode.get("option", "");
                            }
                        }
                        // per-PV metadata
                        if (pvNode.hasChild("metadata"))
                        {
                            auto mv = pvNode.subConfig("metadata");
                            if (!mv.empty())
                            {
                                const auto& raw = mv[0].raw();
                                if (!raw.invalid() && raw.is_map())
                                {
                                    for (const auto child : raw.children())
                                    {
                                        if (child.has_key() && child.has_val())
                                        {
                                            std::string k{child.key().str, child.key().len};
                                            std::string v;
                                            child >> v;
                                            pv.metadata.emplace_back(k, v);
                                        }
                                    }
                                }
                            }
                        }
                        if (!pv.name.empty())
                            r.pvs.push_back(std::move(pv));
                    }
                }
                st.readers.push_back(std::move(r));
            }
        }
    }

    // routing
    if (!cfg.hasChild("routing"))
        return;
    auto rv = cfg.subConfig("routing");
    if (rv.empty())
        return;
    const Config& routing = rv[0];
    const auto&   rawRouting = routing.raw();
    if (rawRouting.invalid() || !rawRouting.is_map())
        return;

    st.routing_all_to_all = false;
    for (const auto child : rawRouting.children())
    {
        if (!child.has_key())
            continue;
        const auto   keyView = child.key();
        RoutingEntry re;
        re.writer_name = std::string{keyView.str, keyView.len};

        if (child.has_child("from"))
        {
            auto fromNode = child["from"];
            if (fromNode.is_seq())
            {
                for (const auto entry : fromNode.children())
                {
                    if (entry.has_val())
                    {
                        std::string v;
                        entry >> v;
                        re.from_readers.push_back(std::move(v));
                    }
                }
            }
        }
        if (child.has_child("include"))
        {
            auto incNode = child["include"];
            if (incNode.is_seq())
            {
                for (const auto entry : incNode.children())
                {
                    if (entry.has_val())
                    {
                        std::string v;
                        entry >> v;
                        re.include_globs.push_back(std::move(v));
                    }
                }
            }
        }
        if (child.has_child("exclude"))
        {
            auto excNode = child["exclude"];
            if (excNode.is_seq())
            {
                for (const auto entry : excNode.children())
                {
                    if (entry.has_val())
                    {
                        std::string v;
                        entry >> v;
                        re.exclude_globs.push_back(std::move(v));
                    }
                }
            }
        }
        st.routing.push_back(std::move(re));
    }
}

} // namespace mldp_pvxs_driver::config::wizard_internal

// ─────────────────────────────────────────────────────────────────────────────
// Non-FTXUI stub — only compiled when wizard is disabled
// ─────────────────────────────────────────────────────────────────────────────

#ifndef MLDP_WIZARD_ENABLED

namespace mldp_pvxs_driver::config {

int runWizard(const std::string&, const std::string&)
{
    std::cerr << "Config wizard not available (build without -DMLDP_WIZARD=ON).\n";
    return 1;
}

} // namespace mldp_pvxs_driver::config

#endif // !MLDP_WIZARD_ENABLED
