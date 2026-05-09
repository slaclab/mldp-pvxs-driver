//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <config/wizard.h>
#include "wizard_internal.h"
#include <config/Config.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
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
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return std::stoi(s) > 0;
}

bool isNonNegInt(const std::string& s)
{
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

static std::string ind(int n) { return std::string(static_cast<std::size_t>(n * 2), ' '); }

std::string generateYaml(const WizardState& st)
{
    std::ostringstream o;

    // controller
    o << "controller:\n";
    o << ind(1) << "name: " << (st.controller_name.empty() ? "default" : st.controller_name) << "\n\n";

    // writers
    o << "writer:\n";
    if (!st.mldp_writers.empty()) {
        o << ind(1) << "mldp:\n";
        for (const auto& w : st.mldp_writers) {
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
            if (w.creds_type == "none") {
                o << ind(4) << "credentials: none\n";
            } else if (w.creds_type == "ssl") {
                o << ind(4) << "credentials: ssl\n";
            } else {
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
    if (!st.hdf5_writers.empty()) {
        // group by is_merge
        bool any_hdf5       = false;
        bool any_hdf5_merge = false;
        for (const auto& w : st.hdf5_writers) {
            if (w.is_merge) any_hdf5_merge = true;
            else            any_hdf5       = true;
        }
        auto emitHdf5 = [&](bool merge) {
            o << ind(1) << (merge ? "hdf5-merge" : "hdf5") << ":\n";
            for (const auto& w : st.hdf5_writers) {
                if (w.is_merge != merge) continue;
                o << ind(2) << "- name: " << w.name << "\n";
                o << ind(3) << "base-path: " << w.base_path << "\n";
                o << ind(3) << "max-file-age-s: " << w.max_file_age_s << "\n";
                o << ind(3) << "max-file-size-mb: " << w.max_file_size_mb << "\n";
                o << ind(3) << "flush-interval-ms: " << w.flush_interval_ms << "\n";
                o << ind(3) << "compression-level: " << w.compression_level << "\n";
            }
        };
        if (any_hdf5)       emitHdf5(false);
        if (any_hdf5_merge) emitHdf5(true);
    }
    o << "\n";

    // readers
    if (!st.readers.empty()) {
        o << "reader:\n";
        for (const auto& r : st.readers) {
            o << ind(1) << "- " << r.reader_type << ":\n";
            o << ind(3) << "- name: " << r.name << "\n";
            if (r.reader_type == "epics-pvxs" || r.reader_type == "epics-base") {
                o << ind(4) << "thread-pool: " << r.thread_pool << "\n";
                o << ind(4) << "column-batch-size: " << r.column_batch_size << "\n";
                if (r.reader_type == "epics-base") {
                    o << ind(4) << "monitor-poll-threads: " << r.monitor_poll_threads << "\n";
                    o << ind(4) << "monitor-poll-interval-ms: " << r.monitor_poll_interval_ms << "\n";
                }
            } else if (r.reader_type == "epics-archiver") {
                o << ind(4) << "hostname: " << r.hostname << "\n";
                o << ind(4) << "mode: " << r.mode << "\n";
                if (r.mode == "historical_once") {
                    o << ind(4) << "start-date: \"" << r.start_date << "\"\n";
                    if (!r.end_date.empty())
                        o << ind(4) << "end-date: \"" << r.end_date << "\"\n";
                } else {
                    o << ind(4) << "poll-interval-sec: " << r.poll_interval_sec << "\n";
                    if (!r.lookback_sec.empty())
                        o << ind(4) << "lookback-sec: " << r.lookback_sec << "\n";
                }
                o << ind(4) << "connect-timeout-sec: " << r.connect_timeout_sec << "\n";
                o << ind(4) << "total-timeout-sec: " << r.total_timeout_sec << "\n";
                o << ind(4) << "batch-duration-sec: " << r.batch_duration_sec << "\n";
                o << ind(4) << "tls-verify-peer: " << r.tls_verify_peer << "\n";
                o << ind(4) << "tls-verify-host: " << r.tls_verify_host << "\n";
            }
            if (!r.pvs.empty()) {
                o << ind(4) << "pvs:\n";
                for (const auto& pv : r.pvs) {
                    if (pv.option_type == "none" || pv.option_type.empty()) {
                        o << ind(5) << "- name: " << pv.name << "\n";
                    } else if (pv.option_type == "scalar") {
                        o << ind(5) << "- name: " << pv.name << "\n";
                        o << ind(6) << "option: \"" << pv.option_value << "\"\n";
                    } else if (pv.option_type == "slac-bsas-table") {
                        o << ind(5) << "- name: " << pv.name << "\n";
                        o << ind(6) << "option:\n";
                        o << ind(7) << "type: slac-bsas-table\n";
                        o << ind(7) << "tsSeconds: " << pv.ts_seconds << "\n";
                        o << ind(7) << "tsNanos: " << pv.ts_nanos << "\n";
                    }
                }
            }
        }
        o << "\n";
    }

    // metrics
    if (st.metrics_enabled) {
        o << "metrics:\n";
        o << ind(1) << "endpoint: \"" << st.metrics_endpoint << "\"\n";
        o << ind(1) << "scan-interval-seconds: " << st.metrics_interval << "\n\n";
    }

    // routing
    if (!st.routing_all_to_all && !st.routing.empty()) {
        o << "routing:\n";
        for (const auto& re : st.routing) {
            o << ind(1) << re.writer_name << ":\n";
            if (!re.from_readers.empty()) {
                o << ind(2) << "from:\n";
                for (const auto& r : re.from_readers)
                    o << ind(3) << "- " << r << "\n";
            }
            if (!re.include_globs.empty()) {
                o << ind(2) << "include:\n";
                for (const auto& g : re.include_globs)
                    o << ind(3) << "- \"" << g << "\"\n";
            }
            if (!re.exclude_globs.empty()) {
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
    try {
        cfg = Config::configFromFile(path);
    } catch (...) {
        std::cerr << "Warning: could not parse '" << path << "' for amend mode — starting fresh.\n";
        return;
    }

    // controller
    if (cfg.hasChild("controller")) {
        auto cv = cfg.subConfig("controller");
        if (!cv.empty()) st.controller_name = cv[0].get("name", "default");
    }

    // metrics
    if (cfg.hasChild("metrics")) {
        auto mv = cfg.subConfig("metrics");
        if (!mv.empty()) {
            const auto& m = mv[0];
            st.metrics_enabled  = true;
            st.metrics_endpoint = m.get("endpoint", "0.0.0.0:9464");
            st.metrics_interval = std::to_string(m.getInt("scan-interval-seconds", 1));
        }
    }

    if (!cfg.hasChild("writer")) return;
    auto writerVec = cfg.subConfig("writer");
    if (writerVec.empty()) return;
    const Config& writer = writerVec[0];

    // mldp writers
    for (const auto& inst : writer.subConfig("mldp")) {
        MldpWriterConfig w;
        w.name               = inst.get("name", "");
        w.thread_pool        = std::to_string(inst.getInt("thread-pool", 1));
        w.stream_max_bytes   = std::to_string(inst.getInt("stream-max-bytes", 2097152));
        w.stream_max_age_ms  = std::to_string(inst.getInt("stream-max-age-ms", 200));
        if (inst.hasChild("mldp-pool")) {
            auto pv = inst.subConfig("mldp-pool");
            if (!pv.empty()) {
                const auto& p = pv[0];
                w.provider_name = p.get("provider-name", "");
                w.provider_desc = p.get("provider-description", "");
                w.ingestion_url = p.get("ingestion-url", "");
                w.query_url     = p.get("query-url", "");
                w.min_conn      = std::to_string(p.getInt("min-conn", 1));
                w.max_conn      = std::to_string(p.getInt("max-conn", 4));
                std::string cred = p.get("credentials", "ssl");
                if (cred == "none" || cred == "ssl") {
                    w.creds_type = cred;
                } else {
                    w.creds_type = "custom-tls";
                }
            }
        }
        st.mldp_writers.push_back(std::move(w));
    }

    // hdf5 / hdf5-merge writers
    auto loadHdf5 = [&](const std::string& tag, bool is_merge) {
        for (const auto& inst : writer.subConfig(tag)) {
            Hdf5WriterConfig w;
            w.is_merge          = is_merge;
            w.name              = inst.get("name", "");
            w.base_path         = inst.get("base-path", "");
            w.max_file_age_s    = std::to_string(inst.getInt("max-file-age-s", 3600));
            w.max_file_size_mb  = std::to_string(inst.getInt("max-file-size-mb", 512));
            w.flush_interval_ms = std::to_string(inst.getInt("flush-interval-ms", 1000));
            w.compression_level = std::to_string(inst.getInt("compression-level", 0));
            st.hdf5_writers.push_back(std::move(w));
        }
    };
    loadHdf5("hdf5", false);
    loadHdf5("hdf5-merge", true);

    // readers
    if (!cfg.hasChild("reader")) return;
    for (const auto& rentry : cfg.subConfig("reader")) {
        static const std::vector<std::string> rtypes =
            {"epics-pvxs", "epics-base", "epics-archiver"};
        for (const auto& rtype : rtypes) {
            if (!rentry.hasChild(rtype)) continue;
            for (const auto& inst : rentry.subConfig(rtype)) {
                EpicsReaderConfig r;
                r.reader_type        = rtype;
                r.name               = inst.get("name", "");
                r.thread_pool        = std::to_string(inst.getInt("thread-pool", 2));
                r.column_batch_size  = std::to_string(inst.getInt("column-batch-size", 50));
                if (rtype == "epics-base") {
                    r.monitor_poll_threads     = std::to_string(inst.getInt("monitor-poll-threads", 2));
                    r.monitor_poll_interval_ms = std::to_string(inst.getInt("monitor-poll-interval-ms", 5));
                }
                if (rtype == "epics-archiver") {
                    r.hostname            = inst.get("hostname", "");
                    r.mode                = inst.get("mode", "historical_once");
                    r.start_date          = inst.get("start-date", "");
                    r.end_date            = inst.get("end-date", "");
                    r.poll_interval_sec   = std::to_string(inst.getInt("poll-interval-sec", 0));
                    r.lookback_sec        = std::to_string(inst.getInt("lookback-sec", 0));
                    r.connect_timeout_sec = std::to_string(inst.getInt("connect-timeout-sec", 30));
                    r.total_timeout_sec   = std::to_string(inst.getInt("total-timeout-sec", 300));
                    r.batch_duration_sec  = std::to_string(inst.getInt("batch-duration-sec", 1));
                    r.tls_verify_peer     = inst.getBool("tls-verify-peer", true) ? "true" : "false";
                    r.tls_verify_host     = inst.getBool("tls-verify-host", true) ? "true" : "false";
                }
                if (inst.hasChild("pvs")) {
                    for (const auto& pvNode : inst.subConfig("pvs")) {
                        PvEntry pv;
                        pv.name        = pvNode.get("name", "");
                        pv.option_type = "none";
                        if (pvNode.hasChild("option")) {
                            // could be scalar string or map — just treat as scalar for amend
                            pv.option_type  = "scalar";
                            pv.option_value = pvNode.get("option", "");
                        }
                        if (!pv.name.empty()) r.pvs.push_back(std::move(pv));
                    }
                }
                st.readers.push_back(std::move(r));
            }
        }
    }
}

} // namespace mldp_pvxs_driver::config::wizard_internal

// ─────────────────────────────────────────────────────────────────────────────
// Interactive phases — require FTXUI
// ─────────────────────────────────────────────────────────────────────────────

#ifdef MLDP_WIZARD_ENABLED

#include "wizard_ui.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace mldp_pvxs_driver::config {

using namespace ftxui;
using namespace wizard_ui;
using namespace wizard_internal;

// ─────────────────────────────────────────────────────────────────────────────
// Simple blocking prompt helpers (single-screen interactions)
// ─────────────────────────────────────────────────────────────────────────────

// Run a single input screen, returns user-entered value (or def on Escape)
static std::string promptInput(
    const std::string& phase_title,
    int phase, int total,
    const std::string& field_label,
    const std::string& def_val,
    std::function<std::string(const std::string&)> validator = {})
{
    auto screen = ScreenInteractive::TerminalOutput();
    std::string value = def_val;

    auto input_field = InputField(field_label, &value, validator);

    auto ok_btn = Button("OK", [&]{ screen.Exit(); }, ButtonOption::Simple());
    auto layout = Container::Vertical({input_field, ok_btn});

    auto renderer = Renderer(layout, [&]{
        return vbox({
            PhaseHeader(phase_title, phase, total),
            separator(),
            input_field->Render() | border,
            ok_btn->Render(),
        });
    });

    // Allow Enter to confirm
    auto wrapped = CatchEvent(renderer, [&](Event ev) -> bool {
        if (ev == Event::Return) { screen.Exit(); return true; }
        return false;
    });

    screen.Loop(wrapped);
    return value;
}

// Run a yes/no screen, returns true=yes
static bool promptYesNo(
    const std::string& phase_title,
    int phase, int total,
    const std::string& question,
    bool default_yes)
{
    auto screen = ScreenInteractive::TerminalOutput();
    bool result = default_yes;

    auto yes_btn = Button("Yes", [&]{ result = true;  screen.Exit(); });
    auto no_btn  = Button("No",  [&]{ result = false; screen.Exit(); });
    auto btns    = Container::Horizontal({yes_btn, no_btn});

    auto renderer = Renderer(btns, [&]{
        return vbox({
            PhaseHeader(phase_title, phase, total),
            separator(),
            text(question) | bold,
            hbox({
                text("Default: "), text(default_yes ? "Yes" : "No") | color(Color::Yellow),
            }),
            separator(),
            btns->Render(),
        });
    });

    // Allow Enter to accept default
    auto wrapped = CatchEvent(renderer, [&](Event ev) -> bool {
        if (ev == Event::Return) { screen.Exit(); return true; }
        return false;
    });

    screen.Loop(wrapped);
    return result;
}

// Run a menu-choice screen; returns index 0..choices.size()-1
static int promptMenu(
    const std::string& phase_title,
    int phase, int total,
    const std::string& question,
    const std::vector<std::string>& choices,
    int default_idx = 0)
{
    auto screen = ScreenInteractive::TerminalOutput();
    int  selected = default_idx;

    auto menu   = TypeMenu(&choices, &selected);
    auto ok_btn = Button("OK", [&]{ screen.Exit(); }, ButtonOption::Simple());
    auto layout = Container::Vertical({menu, ok_btn});

    auto renderer = Renderer(layout, [&]{
        return vbox({
            PhaseHeader(phase_title, phase, total),
            separator(),
            text(question) | bold,
            menu->Render() | border,
            ok_btn->Render(),
        });
    });

    screen.Loop(renderer);
    return selected;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase implementations
// ─────────────────────────────────────────────────────────────────────────────

static void phase1_controller(WizardState& st)
{
    st.controller_name = promptInput(
        "Controller", 1, 6,
        "Controller name (optional)",
        st.controller_name.empty() ? "default" : st.controller_name);
    if (st.controller_name.empty()) st.controller_name = "default";
}

static void phase2_writers(WizardState& st)
{
    std::set<std::string> used_names;
    for (const auto& w : st.mldp_writers)  used_names.insert(w.name);
    for (const auto& w : st.hdf5_writers)  used_names.insert(w.name);

    auto unique_name_validator = [&](const std::string& v) -> std::string {
        if (v.empty()) return "name required";
        if (used_names.count(v)) return "name '" + v + "' already used";
        return "";
    };

    bool add_another = true;
    while (add_another) {
        static const std::vector<std::string> writer_types = {"mldp", "hdf5", "hdf5-merge"};
        int type_idx = promptMenu("Writers", 2, 6, "Select writer type:", writer_types);

        if (type_idx == 0) {
            // ── mldp ──
            MldpWriterConfig w;

            w.name = promptInput("Writers", 2, 6, "Writer name",
                w.name, unique_name_validator);
            used_names.insert(w.name);

            w.thread_pool = promptInput("Writers", 2, 6, "thread-pool",
                w.thread_pool, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.stream_max_bytes = promptInput("Writers", 2, 6, "stream-max-bytes",
                w.stream_max_bytes, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.stream_max_age_ms = promptInput("Writers", 2, 6, "stream-max-age-ms",
                w.stream_max_age_ms, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.provider_name = promptInput("Writers", 2, 6, "mldp-pool provider-name",
                w.provider_name, [](const std::string& v){ return v.empty() ? "required" : ""; });
            w.provider_desc = promptInput("Writers", 2, 6, "mldp-pool provider-description (optional)",
                w.provider_desc);
            w.ingestion_url = promptInput("Writers", 2, 6, "mldp-pool ingestion-url",
                w.ingestion_url, [](const std::string& v){ return v.empty() ? "required" : ""; });
            w.query_url = promptInput("Writers", 2, 6, "mldp-pool query-url (optional)",
                w.query_url);
            w.min_conn = promptInput("Writers", 2, 6, "mldp-pool min-conn",
                w.min_conn, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.max_conn = promptInput("Writers", 2, 6, "mldp-pool max-conn",
                w.max_conn, [&w](const std::string& v){
                    if (!isPositiveInt(v)) return std::string("must be > 0");
                    if (std::stoi(v) < std::stoi(w.min_conn)) return std::string("must be >= min-conn");
                    return std::string("");
                });

            static const std::vector<std::string> cred_types = {"ssl", "none", "custom-tls"};
            int cred_idx = 0;
            for (int i = 0; i < (int)cred_types.size(); ++i) {
                if (cred_types[i] == w.creds_type) { cred_idx = i; break; }
            }
            cred_idx = promptMenu("Writers", 2, 6, "Credentials type:", cred_types, cred_idx);
            w.creds_type = cred_types[cred_idx];

            if (w.creds_type == "custom-tls") {
                w.pem_cert_chain  = promptInput("Writers", 2, 6, "pem-cert-chain path (optional)",  w.pem_cert_chain);
                w.pem_private_key = promptInput("Writers", 2, 6, "pem-private-key path (optional)", w.pem_private_key);
                w.pem_root_certs  = promptInput("Writers", 2, 6, "pem-root-certs path (optional)",  w.pem_root_certs);
            }

            st.mldp_writers.push_back(std::move(w));
            std::cout << "[Added: mldp \"" << st.mldp_writers.back().name << "\"]\n";

        } else {
            // ── hdf5 / hdf5-merge ──
            Hdf5WriterConfig w;
            w.is_merge = (type_idx == 2);

            w.name = promptInput("Writers", 2, 6, "Writer name",
                w.name, unique_name_validator);
            used_names.insert(w.name);

            w.base_path = promptInput("Writers", 2, 6, "base-path",
                w.base_path, [](const std::string& v){ return v.empty() ? "required" : ""; });
            w.max_file_age_s = promptInput("Writers", 2, 6, "max-file-age-s",
                w.max_file_age_s, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.max_file_size_mb = promptInput("Writers", 2, 6, "max-file-size-mb",
                w.max_file_size_mb, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.flush_interval_ms = promptInput("Writers", 2, 6, "flush-interval-ms",
                w.flush_interval_ms, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            w.compression_level = promptInput("Writers", 2, 6, "compression-level (0-9)",
                w.compression_level, [](const std::string& v){
                    if (!isNonNegInt(v)) return std::string("must be integer 0-9");
                    int n = std::stoi(v);
                    return (n >= 0 && n <= 9) ? std::string("") : std::string("must be in [0,9]");
                });

            std::string type_str = w.is_merge ? "hdf5-merge" : "hdf5";
            st.hdf5_writers.push_back(std::move(w));
            std::cout << "[Added: " << type_str << " \"" << st.hdf5_writers.back().name << "\"]\n";
        }

        // guard: at least one writer must exist
        bool has_writers = !st.mldp_writers.empty() || !st.hdf5_writers.empty();
        if (!has_writers) {
            std::cout << "At least one writer required — adding another.\n";
            add_another = true;
        } else {
            add_another = promptYesNo("Writers", 2, 6, "Add another writer?", false);
        }
    }
}

static void phase3_readers(WizardState& st)
{
    std::set<std::string> used_names;
    for (const auto& r : st.readers) used_names.insert(r.name);

    auto unique_name_validator = [&](const std::string& v) -> std::string {
        if (v.empty()) return "name required";
        if (used_names.count(v)) return "name '" + v + "' already used";
        return "";
    };

    bool add_another = true;
    while (add_another) {
        static const std::vector<std::string> reader_types =
            {"epics-pvxs", "epics-base", "epics-archiver"};
        int type_idx = promptMenu("Readers", 3, 6, "Select reader type:", reader_types);

        EpicsReaderConfig r;
        r.reader_type = reader_types[type_idx];

        r.name = promptInput("Readers", 3, 6, "Reader name",
            r.name, unique_name_validator);
        used_names.insert(r.name);

        if (r.reader_type == "epics-pvxs" || r.reader_type == "epics-base") {
            r.thread_pool = promptInput("Readers", 3, 6, "thread-pool",
                r.thread_pool, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            r.column_batch_size = promptInput("Readers", 3, 6, "column-batch-size",
                r.column_batch_size, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            if (r.reader_type == "epics-base") {
                r.monitor_poll_threads = promptInput("Readers", 3, 6, "monitor-poll-threads",
                    r.monitor_poll_threads, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
                r.monitor_poll_interval_ms = promptInput("Readers", 3, 6, "monitor-poll-interval-ms",
                    r.monitor_poll_interval_ms, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            }

        } else {
            // epics-archiver
            r.hostname = promptInput("Readers", 3, 6, "hostname (host:port)",
                r.hostname, [](const std::string& v){
                    return (v.find(':') != std::string::npos && v.size() > 2) ? "" : "expected host:port";
                });

            static const std::vector<std::string> modes = {"historical_once", "periodic_tail"};
            int mode_idx = (r.mode == "periodic_tail") ? 1 : 0;
            mode_idx = promptMenu("Readers", 3, 6, "Mode:", modes, mode_idx);
            r.mode = modes[mode_idx];

            if (r.mode == "historical_once") {
                r.start_date = promptInput("Readers", 3, 6, "start-date (ISO 8601, required)",
                    r.start_date, [](const std::string& v){
                        return isValidIso8601(v) ? "" : "expected ISO 8601 (e.g. 2026-01-01T00:00:00Z)";
                    });
                r.end_date = promptInput("Readers", 3, 6, "end-date (ISO 8601, optional)",
                    r.end_date, [](const std::string& v){
                        if (v.empty()) return std::string("");
                        return isValidIso8601(v) ? std::string("") : std::string("expected ISO 8601");
                    });
            } else {
                r.poll_interval_sec = promptInput("Readers", 3, 6, "poll-interval-sec (required)",
                    r.poll_interval_sec, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
                r.lookback_sec = promptInput("Readers", 3, 6, "lookback-sec (default: poll-interval-sec)",
                    r.lookback_sec.empty() ? r.poll_interval_sec : r.lookback_sec);
            }

            r.connect_timeout_sec = promptInput("Readers", 3, 6, "connect-timeout-sec",
                r.connect_timeout_sec, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
            r.total_timeout_sec = promptInput("Readers", 3, 6, "total-timeout-sec (0=infinite)",
                r.total_timeout_sec, [&r](const std::string& v){
                    if (!isNonNegInt(v)) return std::string("must be non-negative integer");
                    int total = std::stoi(v);
                    int conn  = std::stoi(r.connect_timeout_sec);
                    if (total > 0 && total < conn) return std::string("must be >= connect-timeout-sec");
                    return std::string("");
                });
            r.batch_duration_sec = promptInput("Readers", 3, 6, "batch-duration-sec",
                r.batch_duration_sec, [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });

            bool tls_peer = promptYesNo("Readers", 3, 6, "tls-verify-peer?", r.tls_verify_peer == "true");
            bool tls_host = promptYesNo("Readers", 3, 6, "tls-verify-host?", r.tls_verify_host == "true");
            r.tls_verify_peer = tls_peer ? "true" : "false";
            r.tls_verify_host = tls_host ? "true" : "false";
        }

        // PV entry
        bool add_pvs = promptYesNo("Readers", 3, 6,
            "Add PVs? (optional — some deployments omit)", true);

        if (add_pvs) {
            bool add_pv = true;
            while (add_pv) {
                auto screen2 = ScreenInteractive::TerminalOutput();
                std::string pv_input;

                auto pv_field = InputField("PV name (or paste multi-line block)", &pv_input);
                auto ok_btn   = Button("Add", [&]{ screen2.Exit(); }, ButtonOption::Simple());
                auto layout2  = Container::Vertical({pv_field, ok_btn});

                bool bulk_mode = false;
                int  bulk_count = 0;

                auto renderer2 = Renderer(layout2, [&]{
                    return vbox({
                        PhaseHeader("Readers — PV entry", 3, 6),
                        separator(),
                        text("Reader: " + r.name) | bold,
                        text("PVs added so far: " + std::to_string(r.pvs.size())),
                        separator(),
                        bulk_mode
                            ? text("[" + std::to_string(bulk_count) + " PVs pasted]") | color(Color::Green)
                            : pv_field->Render() | border,
                        ok_btn->Render(),
                    });
                });

                // Detect paste (newline in input)
                auto ev_catcher = CatchEvent(renderer2, [&](Event ev) -> bool {
                    if (ev == Event::Return && !bulk_mode) {
                        // If multiline was pasted before Enter
                        if (pv_input.find('\n') != std::string::npos) {
                            // bulk
                            std::istringstream ss(pv_input);
                            std::string line;
                            std::set<std::string> existing;
                            for (const auto& p : r.pvs) existing.insert(p.name);
                            bulk_count = 0;
                            while (std::getline(ss, line)) {
                                // trim
                                auto b = line.find_first_not_of(" \t\r");
                                auto e = line.find_last_not_of(" \t\r");
                                if (b == std::string::npos) continue;
                                std::string pname = line.substr(b, e - b + 1);
                                if (pname.empty() || existing.count(pname)) continue;
                                PvEntry pv; pv.name = pname; pv.option_type = "none";
                                r.pvs.push_back(pv);
                                existing.insert(pname);
                                ++bulk_count;
                            }
                            bulk_mode = true;
                            pv_input  = "[" + std::to_string(bulk_count) + " PVs pasted]";
                            screen2.Exit();
                            return true;
                        }
                        screen2.Exit();
                        return true;
                    }
                    // Dynamic paste detection (mid-type newline)
                    if (pv_input.find('\n') != std::string::npos && !bulk_mode) {
                        std::istringstream ss(pv_input);
                        std::string line;
                        std::set<std::string> existing;
                        for (const auto& p : r.pvs) existing.insert(p.name);
                        bulk_count = 0;
                        while (std::getline(ss, line)) {
                            auto b = line.find_first_not_of(" \t\r");
                            auto e = line.find_last_not_of(" \t\r");
                            if (b == std::string::npos) continue;
                            std::string pname = line.substr(b, e - b + 1);
                            if (pname.empty() || existing.count(pname)) continue;
                            PvEntry pv; pv.name = pname; pv.option_type = "none";
                            r.pvs.push_back(pv);
                            existing.insert(pname);
                            ++bulk_count;
                        }
                        bulk_mode = true;
                        pv_input  = "[" + std::to_string(bulk_count) + " PVs pasted]";
                        return false;
                    }
                    return false;
                });

                screen2.Loop(ev_catcher);

                if (!bulk_mode && !pv_input.empty()) {
                    // Single PV with option prompt
                    std::string pname = pv_input;
                    // trim
                    auto b = pname.find_first_not_of(" \t\r\n");
                    auto e = pname.find_last_not_of(" \t\r\n");
                    if (b != std::string::npos) pname = pname.substr(b, e - b + 1);

                    if (!pname.empty()) {
                        PvEntry pv;
                        pv.name = pname;

                        if (r.reader_type != "epics-archiver") {
                            static const std::vector<std::string> opt_types =
                                {"none", "scalar", "slac-bsas-table"};
                            int opt_idx = promptMenu("Readers", 3, 6,
                                "Option type for PV '" + pname + "':", opt_types);
                            pv.option_type = opt_types[opt_idx];
                            if (pv.option_type == "scalar") {
                                pv.option_value = promptInput("Readers", 3, 6,
                                    "option value string", "", [](const std::string& v){
                                        return v.empty() ? "required" : "";
                                    });
                            } else if (pv.option_type == "slac-bsas-table") {
                                pv.ts_seconds = promptInput("Readers", 3, 6, "tsSeconds field name", "",
                                    [](const std::string& v){ return v.empty() ? "required" : ""; });
                                pv.ts_nanos   = promptInput("Readers", 3, 6, "tsNanos field name", "",
                                    [](const std::string& v){ return v.empty() ? "required" : ""; });
                            }
                        } else {
                            pv.option_type = "none";
                        }
                        r.pvs.push_back(std::move(pv));
                    }
                }

                add_pv = promptYesNo("Readers", 3, 6, "Add another PV?", false);
            }
        }

        st.readers.push_back(std::move(r));
        const auto& added = st.readers.back();
        std::cout << "[Added: " << added.reader_type << " \"" << added.name
                  << "\" — " << added.pvs.size() << " PVs]\n";

        add_another = promptYesNo("Readers", 3, 6, "Add another reader?", false);
    }
}

static void phase4_metrics(WizardState& st)
{
    bool enable = promptYesNo("Metrics", 4, 6, "Enable Prometheus metrics endpoint?",
        st.metrics_enabled);
    st.metrics_enabled = enable;
    if (!enable) return;

    st.metrics_endpoint = promptInput("Metrics", 4, 6, "Bind address",
        st.metrics_endpoint.empty() ? "0.0.0.0:9464" : st.metrics_endpoint,
        [](const std::string& v){ return v.empty() ? "required" : ""; });
    st.metrics_interval = promptInput("Metrics", 4, 6, "scan-interval-seconds",
        st.metrics_interval.empty() ? "1" : st.metrics_interval,
        [](const std::string& v){ return isPositiveInt(v) ? "" : "must be > 0"; });
}

static void phase5_routing(WizardState& st)
{
    bool all_to_all = promptYesNo("Routing", 5, 6,
        "Use all-to-all routing (every reader → every writer)?",
        st.routing_all_to_all);
    st.routing_all_to_all = all_to_all;
    if (all_to_all) return;

    // Build reader display list
    std::vector<std::string> reader_labels;
    for (const auto& r : st.readers) {
        reader_labels.push_back(
            r.name + "  (" + r.reader_type + ", " +
            std::to_string(r.pvs.size()) + " PVs)");
    }

    // For each writer
    auto configure_for_writer = [&](const std::string& wname, const std::string& wtype) {
        // Reader selection via CheckboxList
        std::vector<int> sel(reader_labels.size(), 0);
        // Restore previous selection if any
        for (const auto& re : st.routing) {
            if (re.writer_name == wname) {
                for (std::size_t i = 0; i < st.readers.size(); ++i) {
                    for (const auto& fr : re.from_readers) {
                        if (fr == st.readers[i].name) sel[i] = 1;
                    }
                }
            }
        }

        auto screen = ScreenInteractive::TerminalOutput();
        auto checklist = MultiSelectList(&reader_labels, &sel);
        auto ok_btn    = Button("OK", [&]{ screen.Exit(); }, ButtonOption::Simple());
        auto layout    = Container::Vertical({checklist, ok_btn});

        auto renderer = Renderer(layout, [&]{
            return vbox({
                PhaseHeader("Routing — " + wname + " (" + wtype + ")", 5, 6),
                separator(),
                text("Select readers to route to this writer:") | bold,
                checklist->Render() | border,
                ok_btn->Render(),
            });
        });
        screen.Loop(renderer);

        RoutingEntry entry;
        entry.writer_name = wname;
        for (std::size_t i = 0; i < st.readers.size(); ++i) {
            if (sel[i] != 0) entry.from_readers.push_back(st.readers[i].name);
        }

        // Glob filters for hdf5/hdf5-merge writers
        if (wtype == "hdf5" || wtype == "hdf5-merge") {
            // Suggest prefix globs from selected readers with PVs
            std::set<std::string> suggested;
            for (std::size_t i = 0; i < st.readers.size(); ++i) {
                if (!sel[i]) continue;
                const auto& r = st.readers[i];
                if (r.pvs.empty()) {
                    std::cout << "  " << r.name << " has no declared PVs — enter glob manually or leave empty.\n";
                    continue;
                }
                // Extract common prefixes (up to first ':')
                for (const auto& pv : r.pvs) {
                    auto pos = pv.name.rfind(':');
                    if (pos != std::string::npos) {
                        suggested.insert(pv.name.substr(0, pos + 1) + "*");
                    }
                }
            }
            for (const auto& s : suggested) {
                bool use = promptYesNo("Routing", 5, 6,
                    "Suggested include glob: " + s + " — use?", true);
                if (use) entry.include_globs.push_back(s);
            }

            bool add_include = promptYesNo("Routing", 5, 6, "Add custom include glob pattern?", false);
            while (add_include) {
                std::string glob = promptInput("Routing", 5, 6, "Include glob pattern", "",
                    [](const std::string& v){ return v.empty() ? "pattern cannot be empty" : ""; });
                if (!glob.empty()) entry.include_globs.push_back(glob);
                add_include = promptYesNo("Routing", 5, 6, "Add another include pattern?", false);
            }

            bool add_exclude = promptYesNo("Routing", 5, 6, "Add exclude glob pattern?", false);
            while (add_exclude) {
                std::string glob = promptInput("Routing", 5, 6, "Exclude glob pattern", "",
                    [](const std::string& v){ return v.empty() ? "pattern cannot be empty" : ""; });
                if (!glob.empty()) entry.exclude_globs.push_back(glob);
                add_exclude = promptYesNo("Routing", 5, 6, "Add another exclude pattern?", false);
            }
        }

        // Remove old routing entry for this writer (amend mode) and replace
        st.routing.erase(
            std::remove_if(st.routing.begin(), st.routing.end(),
                [&wname](const RoutingEntry& e){ return e.writer_name == wname; }),
            st.routing.end());
        st.routing.push_back(std::move(entry));
    };

    for (const auto& w : st.mldp_writers)  configure_for_writer(w.name, "mldp");
    for (const auto& w : st.hdf5_writers)  configure_for_writer(w.name, w.is_merge ? "hdf5-merge" : "hdf5");
}

static int phase6_review_save(WizardState& st, const std::string& output_path)
{
    std::string yaml = generateYaml(st);

    // Build summary lines
    std::vector<std::string> summary;
    {
        std::string writers_line = "Writers : ";
        for (const auto& w : st.mldp_writers)  writers_line += w.name + " (mldp), ";
        for (const auto& w : st.hdf5_writers)  writers_line += w.name + (w.is_merge ? " (hdf5-merge), " : " (hdf5), ");
        if (!writers_line.empty() && writers_line.back() == ' ') writers_line.resize(writers_line.size() - 2);
        summary.push_back(writers_line);

        std::string readers_line = "Readers : ";
        for (const auto& r : st.readers) {
            readers_line += r.name + " (" + r.reader_type + ", " +
                            std::to_string(r.pvs.size()) + " PVs), ";
        }
        if (!readers_line.empty() && readers_line.back() == ' ') readers_line.resize(readers_line.size() - 2);
        summary.push_back(readers_line);

        if (st.metrics_enabled)
            summary.push_back("Metrics : " + st.metrics_endpoint);
        else
            summary.push_back("Metrics : disabled");

        if (st.routing_all_to_all) {
            summary.push_back("Routing : all-to-all");
        } else {
            for (const auto& re : st.routing) {
                std::string rline = "Routing : " + re.writer_name + " ← ";
                for (const auto& f : re.from_readers) rline += f + " ";
                if (!re.include_globs.empty()) {
                    rline += "[include:";
                    for (const auto& g : re.include_globs) rline += " " + g;
                    rline += "]";
                }
                summary.push_back(rline);
            }
        }
    }

    // Show summary + YAML in a scrollable review screen
    auto screen = ScreenInteractive::TerminalOutput();
    bool confirmed = false;
    std::string save_path = output_path;

    auto save_field = InputField("Output path", &save_path);
    auto ok_btn     = Button("Save", [&]{ confirmed = true;  screen.Exit(); }, ButtonOption::Simple());
    auto skip_btn   = Button("Skip", [&]{ confirmed = false; screen.Exit(); }, ButtonOption::Simple());
    auto btns       = Container::Horizontal({ok_btn, skip_btn});
    auto layout     = Container::Vertical({save_field, btns});

    auto renderer = Renderer(layout, [&]{
        Elements sum_elems;
        for (const auto& l : summary) sum_elems.push_back(text(l));

        return vbox({
            PhaseHeader("Review & Save", 6, 6),
            separator(),
            vbox(std::move(sum_elems)) | border,
            separator(),
            text("Generated YAML:") | bold,
            text(yaml) | border,
            separator(),
            save_field->Render(),
            hbox({ok_btn->Render(), text("  "), skip_btn->Render()}),
        });
    });

    screen.Loop(renderer);

    if (confirmed) {
        std::ofstream f(save_path);
        if (!f) {
            std::cerr << "Error: cannot write to '" << save_path << "'\n";
            return 1;
        }
        f << yaml;
        f.close();
        std::cout << "Saved to: " << save_path << "\n";
    } else {
        // Still print YAML to stdout
        std::cout << yaml;
    }

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────

int runWizard(const std::string& output_path, const std::string& from_path)
{
    WizardState st;

    if (!from_path.empty()) {
        std::cout << "Loading existing config from '" << from_path << "' for amend...\n";
        loadFromConfig(from_path, st);
    }

    phase1_controller(st);
    phase2_writers(st);
    phase3_readers(st);
    phase4_metrics(st);
    phase5_routing(st);
    return phase6_review_save(st, output_path.empty() ? "config.yaml" : output_path);
}

} // namespace mldp_pvxs_driver::config

#else // MLDP_WIZARD_ENABLED

#include <iostream>

namespace mldp_pvxs_driver::config {

int runWizard(const std::string&, const std::string&)
{
    std::cerr << "Config wizard not available (build without -DMLDP_WIZARD=ON).\n";
    return 1;
}

} // namespace mldp_pvxs_driver::config

#endif // MLDP_WIZARD_ENABLED
