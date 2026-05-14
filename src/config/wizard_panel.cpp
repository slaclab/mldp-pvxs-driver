#ifdef MLDP_WIZARD_ENABLED

#include <config/wizard.h>
#include "wizard_internal.h"
#include "wizard_ui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <config/validate.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

using namespace ftxui;
using namespace wizard_ui;

// ─── PanelAppState ────────────────────────────────────────────────────────────

struct PanelAppState {
    WizardState&           wizard;
    std::vector<TreeNode>  tree;
    int                    tree_sel          = 0;
    int                    active_panel      = 0;   // 0=sidebar 1=form 2=help
    std::string            focused_field;
    bool                   dirty             = false;
    bool                   valid             = false;
    std::string            status_msg;
    std::string            output_path;
    bool                   show_add_modal    = false;
    bool                   show_delete_modal = false;
    bool                   show_quit_modal   = false;
};

// ─── BuildTree ────────────────────────────────────────────────────────────────

std::vector<TreeNode> BuildTree(const WizardState& w) {
    std::vector<TreeNode> nodes;

    nodes.push_back({TreeNodeKind::Controller, "Controller", "", -1});

    nodes.push_back({TreeNodeKind::WriterGroup, "Writers", "", -1});
    for (int i = 0; i < static_cast<int>(w.mldp_writers.size()); ++i) {
        std::string lbl = "  ├ " + w.mldp_writers[i].name + " (MLDP)";
        if (i == static_cast<int>(w.mldp_writers.size()) - 1 &&
            w.hdf5_writers.empty())
            lbl = "  └ " + w.mldp_writers[i].name + " (MLDP)";
        nodes.push_back({TreeNodeKind::Writer, lbl, "MLDP", i});
    }
    for (int i = 0; i < static_cast<int>(w.hdf5_writers.size()); ++i) {
        std::string tag = w.hdf5_writers[i].is_merge ? "HDF5-merge" : "HDF5";
        std::string lbl = "  ├ " + w.hdf5_writers[i].name + " (" + tag + ")";
        if (i == static_cast<int>(w.hdf5_writers.size()) - 1)
            lbl = "  └ " + w.hdf5_writers[i].name + " (" + tag + ")";
        nodes.push_back({TreeNodeKind::Writer, lbl, tag, i});
    }

    nodes.push_back({TreeNodeKind::ReaderGroup, "Readers", "", -1});
    for (int i = 0; i < static_cast<int>(w.readers.size()); ++i) {
        std::string lbl = "  ├ " + w.readers[i].name +
                          " (" + w.readers[i].reader_type + ")";
        if (i == static_cast<int>(w.readers.size()) - 1)
            lbl = "  └ " + w.readers[i].name +
                  " (" + w.readers[i].reader_type + ")";
        std::string tag = w.readers[i].reader_type; // "epics-pvxs" etc
        nodes.push_back({TreeNodeKind::Reader, lbl, tag, i});
    }

    nodes.push_back({TreeNodeKind::MetricsGroup, "Metrics", "", -1});
    nodes.push_back({TreeNodeKind::RoutingGroup, "Routing", "", -1});

    return nodes;
}

// ─── Help text map ────────────────────────────────────────────────────────────

static const std::map<std::pair<TreeNodeKind, std::string>, std::string> kHelpMap = {
    {{TreeNodeKind::Controller, "name"},
     "Controller name.\nMust be non-empty."},
    {{TreeNodeKind::Writer, "name"},
     "Writer instance name.\nMust be unique."},
    {{TreeNodeKind::Writer, "thread_pool"},
     "thread-pool\nNumber of worker threads\nfor ingestion. Min 1."},
    {{TreeNodeKind::Writer, "stream_max_bytes"},
     "stream-max-bytes\nMax bytes per stream\nchunk. Default 2 MB."},
    {{TreeNodeKind::Writer, "stream_max_age_ms"},
     "stream-max-age-ms\nMax age of buffered\ndata in ms."},
    {{TreeNodeKind::Writer, "provider_name"},
     "provider-name\nMLDP provider identifier."},
    {{TreeNodeKind::Writer, "ingestion_url"},
     "ingestion-url\ngRPC endpoint for data\ningestion."},
    {{TreeNodeKind::Writer, "query_url"},
     "query-url\ngRPC endpoint for\ndata queries."},
    {{TreeNodeKind::Writer, "base_path"},
     "base-path\nDirectory for HDF5\noutput files."},
    {{TreeNodeKind::Writer, "max_file_age_s"},
     "max-file-age-s\nMax age of HDF5 file\nbefore rotation (s)."},
    {{TreeNodeKind::Reader, "name"},
     "Reader instance name.\nMust be unique."},
    {{TreeNodeKind::Reader, "thread_pool"},
     "thread-pool\nWorker threads for\nEPICS monitoring."},
    {{TreeNodeKind::Reader, "hostname"},
     "hostname\nArchiver appliance\nhostname or IP."},
    {{TreeNodeKind::MetricsGroup, "enabled"},
     "metrics enabled\nExpose Prometheus\nmetrics endpoint."},
    {{TreeNodeKind::MetricsGroup, "endpoint"},
     "metrics endpoint\nHost:port for the\nPrometheus scrape."},
    {{TreeNodeKind::RoutingGroup, "routing_all_to_all"},
     "routing all-to-all\nConnect every reader\nto every writer."},
};

std::string GetHelpText(TreeNodeKind kind, const std::string& field) {
    auto it = kHelpMap.find({kind, field});
    if (it != kHelpMap.end()) return it->second;
    // Fallback: return first help text for the kind
    for (auto& [k, v] : kHelpMap)
        if (k.first == kind) return v;
    return "";
}

// ─── SidebarPanel ─────────────────────────────────────────────────────────────

namespace wizard_ui {

Component SidebarPanel(const std::vector<TreeNode>* nodes, int* selected_index) {
    auto labels = std::make_shared<std::vector<std::string>>();
    for (auto& n : *nodes) labels->push_back(n.label);

    MenuOption opt;
    opt.entries_option.transform = [nodes, labels](EntryState es) -> Element {
        // Sync labels from nodes each render (ensures add/delete reflects)
        if (labels->size() != nodes->size()) {
            labels->resize(nodes->size());
            for (size_t i = 0; i < nodes->size(); ++i)
                (*labels)[i] = (*nodes)[i].label;
        }
        Element e = text(es.label);
        if (es.focused) e = e | bgcolor(Color::Cyan) | color(Color::Black);
        else if (es.active) e = e | bgcolor(Color::Blue) | color(Color::White);
        return e;
    };

    return Menu(labels.get(), selected_index, opt);
}

} // namespace wizard_ui

// ─── TitleBar ─────────────────────────────────────────────────────────────────

Element TitleBar(const std::string& output_path) {
    return hbox({
        text(" pvxs-driver config ") | bold,
        text("─── ") | dim,
        text(output_path) | color(Color::Yellow),
        text(" ─ ") | dim,
        text("[s]") | bold, text("ave "),
        text("[v]") | bold, text("alidate "),
        text("[q]") | bold, text("uit "),
    }) | bgcolor(Color::Blue) | color(Color::White);
}

// ─── StatusBar ────────────────────────────────────────────────────────────────

Element StatusBar(const WizardState& w, bool valid, bool dirty,
                  const std::string& msg) {
    int n_mldp = static_cast<int>(w.mldp_writers.size());
    int n_hdf5 = static_cast<int>(w.hdf5_writers.size());
    int n_read = static_cast<int>(w.readers.size());
    std::string writers_s = "Writers: " + std::to_string(n_mldp + n_hdf5);
    std::string readers_s = "Readers: " + std::to_string(n_read);
    std::string metrics_s = std::string("Metrics: ") + (w.metrics_enabled ? "on" : "off");
    std::string valid_s   = valid ? "Valid ✓" : "Invalid ✗";
    Color valid_c         = valid ? Color::Green : Color::Red;
    std::string dirty_s   = dirty ? " *" : "";

    return hbox({
        text(" " + writers_s + " │ ") ,
        text(readers_s + " │ "),
        text(metrics_s + " "),
        text(" [a]dd [d]del") | dim,
        filler(),
        text(msg + " ") | color(Color::Yellow),
        text(valid_s) | color(valid_c),
        text(dirty_s + " "),
    }) | bgcolor(Color::Default);
}

// ─── Form implementations ─────────────────────────────────────────────────────

static Component MakeControllerForm(WizardState* w, PanelAppState* state) {
    using namespace wizard_internal;
    auto on_change = [state](const std::string& field) {
        state->dirty = true;
        state->focused_field = field;
    };

    auto name_field = InputField("Name", &w->controller_name,
        [](const std::string& s) { return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("name"); });

    auto form = Container::Vertical({name_field});

    return Renderer(form, [form] {
        return vbox({
            text("  Basic Settings") | bold,
            separator(),
            form->Render(),
        }) | yframe;
    });
}

static Component MakeMldpWriterForm(MldpWriterConfig* cfg, PanelAppState* state) {
    using namespace wizard_internal;
    auto on_change = [state](const std::string& field) {
        state->dirty = true;
        state->focused_field = field;
    };

    // Basic Settings
    auto f_name   = InputField("Name",              &cfg->name,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("name"); });
    auto f_tp     = InputField("Thread Pool",       &cfg->thread_pool,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("thread_pool"); });
    auto f_smb    = InputField("Stream Max Bytes",  &cfg->stream_max_bytes,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("stream_max_bytes"); });
    auto f_sma    = InputField("Stream Max Age ms", &cfg->stream_max_age_ms,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("stream_max_age_ms"); });

    // MLDP Pool
    auto f_pname  = InputField("Provider Name",     &cfg->provider_name,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("provider_name"); });
    auto f_pdesc  = InputField("Provider Desc",     &cfg->provider_desc,
        [](const std::string&){ return ""; },
        [on_change]{ on_change("provider_desc"); });
    auto f_iurl   = InputField("Ingestion URL",     &cfg->ingestion_url,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("ingestion_url"); });
    auto f_qurl   = InputField("Query URL",         &cfg->query_url,
        [](const std::string&){ return ""; },
        [on_change]{ on_change("query_url"); });
    auto f_minc   = InputField("Min Connections",   &cfg->min_conn,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("min_conn"); });
    auto f_maxc   = InputField("Max Connections",   &cfg->max_conn,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("max_conn"); });

    // Credentials — TypeMenu index kept alive via shared_ptr
    static std::vector<std::string> creds_choices = {"none", "ssl", "custom-tls"};
    auto creds_idx = std::make_shared<int>(0);
    // Sync from cfg initial value
    for (int i = 0; i < static_cast<int>(creds_choices.size()); ++i)
        if (creds_choices[i] == cfg->creds_type) { *creds_idx = i; break; }

    auto f_cert   = InputField("PEM Cert Chain",    &cfg->pem_cert_chain,
        [](const std::string&){ return ""; },
        [on_change]{ on_change("pem_cert_chain"); });
    auto f_pkey   = InputField("PEM Private Key",   &cfg->pem_private_key,
        [](const std::string&){ return ""; },
        [on_change]{ on_change("pem_private_key"); });
    auto f_root   = InputField("PEM Root Certs",    &cfg->pem_root_certs,
        [](const std::string&){ return ""; },
        [on_change]{ on_change("pem_root_certs"); });

    auto tls_fields = Container::Vertical({f_cert, f_pkey, f_root});
    auto tls_maybe  = Maybe(tls_fields, [creds_idx]{ return *creds_idx == 2; });

    auto creds_menu = TypeMenu(&creds_choices, creds_idx.get());
    // Wrap to sync string field on render
    auto creds_sync = Renderer(creds_menu, [creds_menu, creds_idx, cfg, on_change] {
        cfg->creds_type = creds_choices[*creds_idx];
        return creds_menu->Render();
    });

    auto form = Container::Vertical({
        f_name, f_tp, f_smb, f_sma,
        f_pname, f_pdesc, f_iurl, f_qurl, f_minc, f_maxc,
        creds_sync, tls_maybe,
    });

    return Renderer(form, [=] {
        return vbox({
            text("  Basic Settings") | bold | underlined,
            f_name->Render(), f_tp->Render(), f_smb->Render(), f_sma->Render(),
            separator(),
            text("  MLDP Pool") | bold | underlined,
            f_pname->Render(), f_pdesc->Render(), f_iurl->Render(), f_qurl->Render(),
            f_minc->Render(), f_maxc->Render(),
            separator(),
            text("  Credentials") | bold | underlined,
            creds_sync->Render(),
            tls_maybe->Render(),
        }) | yframe;
    });
}

static Component MakeHdf5WriterForm(Hdf5WriterConfig* cfg, PanelAppState* state) {
    using namespace wizard_internal;
    auto on_change = [state](const std::string& field) {
        state->dirty = true;
        state->focused_field = field;
    };

    auto f_name   = InputField("Name",                &cfg->name,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("name"); });
    auto f_path   = InputField("Base Path",           &cfg->base_path,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("base_path"); });
    auto f_age    = InputField("Max File Age (s)",    &cfg->max_file_age_s,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("max_file_age_s"); });
    auto f_size   = InputField("Max File Size (MB)",  &cfg->max_file_size_mb,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("max_file_size_mb"); });
    auto f_flush  = InputField("Flush Interval (ms)", &cfg->flush_interval_ms,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("flush_interval_ms"); });
    auto f_comp   = InputField("Compression Level",   &cfg->compression_level,
        [](const std::string& s){ return isNonNegInt(s) ? "" : "Must be non-negative int"; },
        [on_change]{ on_change("compression_level"); });

    std::string merge_label = "Is Merge";
    auto f_merge  = Checkbox(&merge_label, &cfg->is_merge);

    auto form = Container::Vertical({f_name, f_path, f_age, f_size, f_flush, f_comp, f_merge});

    return Renderer(form, [form] {
        return vbox({
            text("  HDF5 Writer Settings") | bold | underlined,
            form->Render(),
        }) | yframe;
    });
}

static Component MakeEpicsReaderForm(EpicsReaderConfig* cfg, PanelAppState* state,
                                     ScreenInteractive* /*screen*/) {
    using namespace wizard_internal;
    auto on_change = [state](const std::string& field) {
        state->dirty = true;
        state->focused_field = field;
    };

    // Reader type menu
    static std::vector<std::string> reader_choices = {"epics-pvxs", "epics-base", "epics-archiver"};
    auto rtype_idx = std::make_shared<int>(0);
    for (int i = 0; i < static_cast<int>(reader_choices.size()); ++i)
        if (reader_choices[i] == cfg->reader_type) { *rtype_idx = i; break; }

    auto f_name   = InputField("Name",                &cfg->name,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("name"); });
    auto f_tp     = InputField("Thread Pool",         &cfg->thread_pool,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("thread_pool"); });
    auto f_cbs    = InputField("Column Batch Size",   &cfg->column_batch_size,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("column_batch_size"); });

    // epics-base only fields
    auto f_mpt    = InputField("Monitor Poll Threads",   &cfg->monitor_poll_threads,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("monitor_poll_threads"); });
    auto f_mpi    = InputField("Monitor Poll Interval ms", &cfg->monitor_poll_interval_ms,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("monitor_poll_interval_ms"); });
    auto base_fields = Container::Vertical({f_mpt, f_mpi});
    auto base_maybe  = Maybe(base_fields, [rtype_idx]{ return *rtype_idx == 1; });

    // epics-archiver only fields
    auto f_host   = InputField("Hostname",           &cfg->hostname,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("hostname"); });

    static std::vector<std::string> mode_choices = {"historical_once", "historical_monitor", "poll_monitor"};
    auto mode_idx = std::make_shared<int>(0);
    for (int i = 0; i < static_cast<int>(mode_choices.size()); ++i)
        if (mode_choices[i] == cfg->mode) { *mode_idx = i; break; }
    auto mode_menu = TypeMenu(&mode_choices, mode_idx.get());
    auto mode_sync = Renderer(mode_menu, [mode_menu, mode_idx, cfg] {
        cfg->mode = mode_choices[*mode_idx];
        return mode_menu->Render();
    });

    auto f_sd     = InputField("Start Date",         &cfg->start_date,
        [](const std::string& s){ return (!s.empty() && !isValidIso8601(s)) ? "Invalid ISO8601" : ""; },
        [on_change]{ on_change("start_date"); });
    auto f_ed     = InputField("End Date",           &cfg->end_date,
        [](const std::string& s){ return (!s.empty() && !isValidIso8601(s)) ? "Invalid ISO8601" : ""; },
        [on_change]{ on_change("end_date"); });
    auto date_fields = Container::Vertical({f_sd, f_ed});
    auto date_maybe  = Maybe(date_fields,
        [mode_idx]{ return *mode_idx == 0 || *mode_idx == 1; });

    auto f_poll   = InputField("Poll Interval (s)",  &cfg->poll_interval_sec,
        [](const std::string& s){ return (!s.empty() && !isPositiveInt(s)) ? "Must be positive int" : ""; },
        [on_change]{ on_change("poll_interval_sec"); });
    auto poll_maybe = Maybe(f_poll, [mode_idx]{ return *mode_idx == 2; });

    auto f_look   = InputField("Lookback (s)",       &cfg->lookback_sec,
        [](const std::string& s){ return (!s.empty() && !isPositiveInt(s)) ? "Must be positive int" : ""; },
        [on_change]{ on_change("lookback_sec"); });
    auto look_maybe = Maybe(f_look, [mode_idx]{ return *mode_idx == 1 || *mode_idx == 2; });

    auto f_cto    = InputField("Connect Timeout (s)", &cfg->connect_timeout_sec,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("connect_timeout_sec"); });
    auto f_tto    = InputField("Total Timeout (s)",   &cfg->total_timeout_sec,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("total_timeout_sec"); });
    auto f_bds    = InputField("Batch Duration (s)",  &cfg->batch_duration_sec,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("batch_duration_sec"); });

    // tls booleans stored as shared_ptr
    auto tls_peer = std::make_shared<bool>(cfg->tls_verify_peer == "true");
    auto tls_host = std::make_shared<bool>(cfg->tls_verify_host == "true");
    std::string peer_lbl = "TLS Verify Peer";
    std::string host_lbl = "TLS Verify Host";
    auto f_tpeer  = Checkbox(&peer_lbl, tls_peer.get());
    auto f_thost  = Checkbox(&host_lbl, tls_host.get());
    auto tls_sync = Renderer(Container::Vertical({f_tpeer, f_thost}),
        [f_tpeer, f_thost, tls_peer, tls_host, cfg, on_change] {
            cfg->tls_verify_peer = *tls_peer ? "true" : "false";
            cfg->tls_verify_host = *tls_host ? "true" : "false";
            return vbox({f_tpeer->Render(), f_thost->Render()});
        });

    auto arch_fields = Container::Vertical({
        f_host, mode_sync, date_maybe, poll_maybe, look_maybe,
        f_cto, f_tto, f_bds, tls_sync,
    });
    auto arch_maybe = Maybe(arch_fields, [rtype_idx]{ return *rtype_idx == 2; });

    // Reader type menu sync
    auto rtype_menu = TypeMenu(&reader_choices, rtype_idx.get());
    auto rtype_sync = Renderer(rtype_menu, [rtype_menu, rtype_idx, cfg] {
        cfg->reader_type = reader_choices[*rtype_idx];
        return rtype_menu->Render();
    });

    // ── PV list ──────────────────────────────────────────────────────────────
    auto pv_sel   = std::make_shared<int>(0);
    auto pv_new   = std::make_shared<std::string>();

    // Build label list for menu dynamically
    auto pv_labels = std::make_shared<std::vector<std::string>>();
    auto rebuild_pv_labels = [cfg, pv_labels] {
        pv_labels->clear();
        for (auto& p : cfg->pvs)
            pv_labels->push_back(p.name + " (" + p.option_type + ")");
        if (pv_labels->empty()) pv_labels->push_back("(no PVs)");
    };
    rebuild_pv_labels();

    auto pv_menu = Menu(pv_labels.get(), pv_sel.get());
    auto pv_menu_event = CatchEvent(pv_menu, [cfg, pv_sel, pv_labels, rebuild_pv_labels,
                                              state](Event ev) {
        if (ev == Event::Character('d') && !cfg->pvs.empty()) {
            int i = *pv_sel;
            if (i >= 0 && i < static_cast<int>(cfg->pvs.size())) {
                cfg->pvs.erase(cfg->pvs.begin() + i);
                rebuild_pv_labels();
                if (*pv_sel >= static_cast<int>(cfg->pvs.size()))
                    *pv_sel = std::max(0, static_cast<int>(cfg->pvs.size()) - 1);
                state->dirty = true;
            }
            return true;
        }
        return false;
    });

    InputOption add_opt;
    auto f_pv_new = Input(pv_new.get(), "new PV name", add_opt);

    auto btn_add  = Button("Add", [cfg, pv_new, pv_labels, rebuild_pv_labels, state] {
        if (!pv_new->empty()) {
            PvEntry e;
            e.name        = *pv_new;
            e.option_type = "none";
            cfg->pvs.push_back(e);
            rebuild_pv_labels();
            pv_new->clear();
            state->dirty = true;
        }
    });

    auto pv_add_row = Container::Horizontal({f_pv_new, btn_add});

    auto pv_section = Container::Vertical({pv_menu_event, pv_add_row});
    auto pv_renderer = Renderer(pv_section, [pv_menu_event, pv_add_row] {
        return vbox({
            text("  PV List  (d=delete)") | bold | underlined,
            pv_menu_event->Render() | frame | size(HEIGHT, LESS_THAN, 8),
            hbox({text("New: "), pv_add_row->Render()}),
        });
    });

    auto form = Container::Vertical({
        f_name, rtype_sync, f_tp, f_cbs,
        base_maybe, arch_maybe,
        pv_renderer,
    });

    return Renderer(form, [=] {
        return vbox({
            text("  EPICS Reader Settings") | bold | underlined,
            f_name->Render(),
            rtype_sync->Render(),
            f_tp->Render(),
            f_cbs->Render(),
            base_maybe->Render(),
            arch_maybe->Render(),
            separator(),
            pv_renderer->Render(),
        }) | yframe;
    });
}

static Component MakeMetricsForm(WizardState* w, PanelAppState* state) {
    using namespace wizard_internal;
    auto on_change = [state](const std::string& field) {
        state->dirty = true;
        state->focused_field = field;
    };

    std::string enabled_lbl = "Enable Metrics";
    auto f_enabled  = Checkbox(&enabled_lbl, &w->metrics_enabled);

    auto f_endpoint = InputField("Endpoint",        &w->metrics_endpoint,
        [](const std::string& s){ return s.empty() ? "Must not be empty" : ""; },
        [on_change]{ on_change("endpoint"); });
    auto f_interval = InputField("Interval (s)",    &w->metrics_interval,
        [](const std::string& s){ return isPositiveInt(s) ? "" : "Must be positive int"; },
        [on_change]{ on_change("interval"); });

    auto detail = Container::Vertical({f_endpoint, f_interval});
    auto detail_maybe = Maybe(detail, [w]{ return w->metrics_enabled; });

    auto form = Container::Vertical({f_enabled, detail_maybe});

    return Renderer(form, [form] {
        return vbox({
            text("  Metrics Settings") | bold | underlined,
            form->Render(),
        }) | yframe;
    });
}

static Component MakeRoutingForm(WizardState* w, PanelAppState* state) {
    std::string all_lbl = "All-to-All Routing";
    auto f_all = Checkbox(&all_lbl, &w->routing_all_to_all);

    auto manual_info = Renderer([w] {
        Elements lines;
        lines.push_back(text("  Manual routing editing: see config file") | dim);
        for (auto& r : w->routing) {
            std::string readers = r.from_readers.empty() ? "all"
                : r.from_readers[0] + (r.from_readers.size() > 1 ? "…" : "");
            lines.push_back(text("  " + readers + " → " + r.writer_name) | dim);
        }
        return vbox(std::move(lines));
    });
    auto manual_maybe = Maybe(manual_info, [w]{ return !w->routing_all_to_all; });

    auto form = Container::Vertical({f_all, manual_maybe});

    return Renderer(form, [form] {
        return vbox({
            text("  Routing Settings") | bold | underlined,
            form->Render(),
        }) | yframe;
    });
}

// ─── HelpPanel stub ──────────────────────────────────────────────────────────

static Component MakeHelpPanel(PanelAppState* state) {
    return Renderer([state] {
        std::string txt = GetHelpText(
            state->tree.empty() ? TreeNodeKind::Controller
                                : state->tree[state->tree_sel].kind,
            state->focused_field);
        Elements lines;
        std::string seg;
        for (char c : txt) {
            if (c == '\n') { lines.push_back(text(seg)); seg.clear(); }
            else seg += c;
        }
        if (!seg.empty()) lines.push_back(text(seg));
        if (lines.empty()) lines.push_back(text(""));
        return vbox(std::move(lines)) | color(Color::GrayLight);
    });
}

// ─── runWizard ────────────────────────────────────────────────────────────────

int runWizard(const std::string& output_path, const std::string& from_path) {
    WizardState wizard;
    if (!from_path.empty()) {
        try { wizard_internal::loadFromConfig(from_path, wizard); }
        catch (...) { /* ignore — start fresh */ }
    }

    auto screen = ScreenInteractive::Fullscreen();

    PanelAppState state{wizard};
    state.output_path = output_path;
    state.tree        = BuildTree(wizard);

    // ── Sidebar ──────────────────────────────────────────────────────────────
    auto sidebar = SidebarPanel(&state.tree, &state.tree_sel);

    // ── Form dispatcher (stacked Maybe components) ────────────────────────
    auto ctrl_form    = MakeControllerForm(&state.wizard, &state);
    auto metrics_form = MakeMetricsForm(&state.wizard, &state);
    auto routing_form = MakeRoutingForm(&state.wizard, &state);

    // MLDP writer forms — one per slot (rebuilt on tree change for now)
    std::vector<Component> mldp_forms, hdf5_forms, reader_forms;
    auto rebuildForms = [&] {
        mldp_forms.clear(); hdf5_forms.clear(); reader_forms.clear();
        for (auto& c : state.wizard.mldp_writers)
            mldp_forms.push_back(MakeMldpWriterForm(
                &const_cast<MldpWriterConfig&>(c), &state));
        for (auto& c : state.wizard.hdf5_writers)
            hdf5_forms.push_back(MakeHdf5WriterForm(
                &const_cast<Hdf5WriterConfig&>(c), &state));
        for (auto& c : state.wizard.readers)
            reader_forms.push_back(MakeEpicsReaderForm(
                &const_cast<EpicsReaderConfig&>(c), &state, &screen));
    };
    rebuildForms();

    auto empty_group = Renderer([]{ return text("  Press [a] to add item."); });

    // ── Dynamic form slot — routes events to the active form ─────────────────
    auto getActiveForm = [&]() -> Component {
        auto& t = state.tree;
        if (t.empty()) return empty_group;
        auto& node = t[state.tree_sel];
        switch (node.kind) {
            case TreeNodeKind::Controller:
                return ctrl_form;
            case TreeNodeKind::WriterGroup:
            case TreeNodeKind::ReaderGroup:
            case TreeNodeKind::MetricsGroup:
            case TreeNodeKind::RoutingGroup:
                return empty_group;
            case TreeNodeKind::Writer:
                if (node.type_tag == "MLDP" &&
                    node.data_index >= 0 &&
                    node.data_index < static_cast<int>(mldp_forms.size()))
                    return mldp_forms[node.data_index];
                if (node.type_tag != "MLDP" &&
                    node.data_index >= 0 &&
                    node.data_index < static_cast<int>(hdf5_forms.size()))
                    return hdf5_forms[node.data_index];
                return empty_group;
            case TreeNodeKind::Reader:
                if (node.data_index >= 0 &&
                    node.data_index < static_cast<int>(reader_forms.size()))
                    return reader_forms[node.data_index];
                return empty_group;
            default:
                return empty_group;
        }
    };

    // Event-routing form container
    class FormSlot : public ComponentBase {
    public:
        explicit FormSlot(std::function<Component()> getter)
            : getter_(std::move(getter)) {}
        Element Render() override {
            auto f = getter_();
            return f ? f->Render() | yframe : text("");
        }
        bool OnEvent(Event ev) override {
            auto f = getter_();
            return f ? f->OnEvent(ev) : false;
        }
        bool Focusable() const override { return true; }
    private:
        std::function<Component()> getter_;
    };

    auto form_container = Make<FormSlot>(getActiveForm);

    // ── Help panel ───────────────────────────────────────────────────────────
    auto help = MakeHelpPanel(&state);

    // ── Root layout ──────────────────────────────────────────────────────────
    auto root_content = Container::Horizontal({sidebar, form_container, help},
                                              &state.active_panel);

    auto root_renderer = Renderer(root_content, [&] {
        return vbox({
            TitleBar(state.output_path),
            hbox({
                sidebar->Render() | size(WIDTH, EQUAL, 26),
                separatorLight(),
                form_container->Render() | flex | yframe,
                separatorLight(),
                help->Render() | size(WIDTH, EQUAL, 30),
            }) | flex,
            separatorLight(),
            StatusBar(state.wizard, state.valid, state.dirty, state.status_msg),
        }) | flex;
    });

    // ── Global key handler ────────────────────────────────────────────────────
    auto app = CatchEvent(root_renderer, [&](Event ev) -> bool {
        // Suppress shortcuts when form panel has focus (panel 1)
        if (state.active_panel == 1) return false;

        if (ev == Event::Character('s')) {
            std::string yaml = wizard_internal::generateYaml(state.wizard);
            std::ofstream f(state.output_path);
            if (f) { f << yaml; state.status_msg = "Saved"; state.dirty = false; }
            else     state.status_msg = "Save failed!";
            return true;
        }
        if (ev == Event::Character('v')) {
            std::string yaml = wizard_internal::generateYaml(state.wizard);
            // validateConfig requires a Config object — write to a temp file and load
            std::string tmp = state.output_path + ".~validate_tmp";
            {
                std::ofstream tf(tmp);
                tf << yaml;
            }
            try {
                Config cfg = Config::configFromFile(tmp);
                std::filesystem::remove(tmp);
                auto diags = validateConfig(cfg);
                if (diags.empty()) {
                    state.valid = true;
                    state.status_msg = "Valid \xe2\x9c\x93";
                } else {
                    state.valid = false;
                    std::string msg = diags[0].message;
                    state.status_msg = "Invalid: " + msg.substr(0, 40);
                }
            } catch (const std::exception& ex) {
                std::filesystem::remove(tmp);
                state.valid = false;
                state.status_msg = std::string("Parse error: ") + ex.what();
            }
            return true;
        }
        if (ev == Event::Character('a')) {
            state.show_add_modal = true;
            return true;
        }
        if (ev == Event::Character('d')) {
            state.show_delete_modal = true;
            return true;
        }
        if (ev == Event::Character('q') || ev == Event::Escape) {
            if (state.dirty) state.show_quit_modal = true;
            else screen.Exit();
            return true;
        }
        return false;
    });

    // ── Add modal ────────────────────────────────────────────────────────────
    auto add_modal = Renderer([&] {
        auto& t = state.tree;
        bool on_reader = !t.empty() && (t[state.tree_sel].kind == TreeNodeKind::ReaderGroup ||
                          t[state.tree_sel].kind == TreeNodeKind::Reader);
        Elements opts;
        if (on_reader) {
            opts.push_back(text(" [p] epics-pvxs "));
            opts.push_back(text(" [b] epics-base "));
            opts.push_back(text(" [a] epics-archiver "));
        } else {
            opts.push_back(text(" [m] MLDP Writer "));
            opts.push_back(text(" [h] HDF5 Writer "));
            opts.push_back(text(" [g] HDF5-merge Writer "));
        }
        opts.push_back(separator());
        opts.push_back(text(" [Esc] cancel ") | dim);
        return vbox(std::move(opts)) | border | size(WIDTH, EQUAL, 36);
    });
    add_modal = CatchEvent(add_modal, [&](Event ev) {
        auto& t = state.tree;
        bool on_reader = !t.empty() && (t[state.tree_sel].kind == TreeNodeKind::ReaderGroup ||
                          t[state.tree_sel].kind == TreeNodeKind::Reader);
        if (ev == Event::Escape) { state.show_add_modal = false; return true; }
        if (!on_reader) {
            if (ev == Event::Character('m')) {
                MldpWriterConfig c; c.name = "mldp_" + std::to_string(state.wizard.mldp_writers.size());
                state.wizard.mldp_writers.push_back(c);
                state.tree = BuildTree(state.wizard);
                state.tree_sel = std::min(state.tree_sel + 1, (int)state.tree.size()-1);
                state.dirty = true; state.show_add_modal = false;
                rebuildForms();
                return true;
            }
            if (ev == Event::Character('h') || ev == Event::Character('g')) {
                Hdf5WriterConfig c;
                c.name = "hdf5_" + std::to_string(state.wizard.hdf5_writers.size());
                c.is_merge = (ev == Event::Character('g'));
                state.wizard.hdf5_writers.push_back(c);
                state.tree = BuildTree(state.wizard);
                state.dirty = true; state.show_add_modal = false;
                rebuildForms();
                return true;
            }
        } else {
            if (ev == Event::Character('p') || ev == Event::Character('b') || ev == Event::Character('a')) {
                EpicsReaderConfig c;
                c.name = "reader_" + std::to_string(state.wizard.readers.size());
                if (ev == Event::Character('p')) c.reader_type = "epics-pvxs";
                else if (ev == Event::Character('b')) c.reader_type = "epics-base";
                else c.reader_type = "epics-archiver";
                state.wizard.readers.push_back(c);
                state.tree = BuildTree(state.wizard);
                state.dirty = true; state.show_add_modal = false;
                rebuildForms();
                return true;
            }
        }
        return false;
    });

    auto delete_modal = Renderer([&] {
        return vbox({
            text(" Delete item? ") | bold | center,
            separator(),
            text(" [y] Yes   [n] No ") | center,
        }) | border | size(WIDTH, EQUAL, 28);
    });
    delete_modal = CatchEvent(delete_modal, [&](Event ev) {
        if (ev == Event::Character('y')) {
            auto& t = state.tree;
            if (!t.empty()) {
                auto& node = t[state.tree_sel];
                int idx = node.data_index;
                if (node.kind == TreeNodeKind::Writer && node.type_tag == "MLDP" &&
                    idx >= 0 && idx < (int)state.wizard.mldp_writers.size()) {
                    state.wizard.mldp_writers.erase(state.wizard.mldp_writers.begin() + idx);
                    state.dirty = true;
                } else if (node.kind == TreeNodeKind::Writer && node.type_tag != "MLDP" &&
                           idx >= 0 && idx < (int)state.wizard.hdf5_writers.size()) {
                    state.wizard.hdf5_writers.erase(state.wizard.hdf5_writers.begin() + idx);
                    state.dirty = true;
                } else if (node.kind == TreeNodeKind::Reader &&
                           idx >= 0 && idx < (int)state.wizard.readers.size()) {
                    state.wizard.readers.erase(state.wizard.readers.begin() + idx);
                    state.dirty = true;
                }
                state.tree = BuildTree(state.wizard);
                state.tree_sel = std::max(0, std::min(state.tree_sel, (int)state.tree.size()-1));
                rebuildForms();
            }
            state.show_delete_modal = false;
            return true;
        }
        if (ev == Event::Character('n') || ev == Event::Escape) {
            state.show_delete_modal = false; return true;
        }
        return false;
    });

    auto quit_modal = Renderer([&] {
        return vbox({
            text(" Unsaved changes — quit anyway? ") | bold | center,
            separator(),
            text(" [y] Yes   [n] No ") | center,
        }) | border | size(WIDTH, EQUAL, 38);
    });
    quit_modal = CatchEvent(quit_modal, [&](Event ev) {
        if (ev == Event::Character('y')) { screen.Exit(); return true; }
        if (ev == Event::Character('n') || ev == Event::Escape) {
            state.show_quit_modal = false; return true;
        }
        return false;
    });

    auto with_modals = Modal(Modal(Modal(app,
        add_modal,    &state.show_add_modal),
        delete_modal, &state.show_delete_modal),
        quit_modal,   &state.show_quit_modal);

    screen.Loop(with_modals);
    return 0;
}

} // namespace mldp_pvxs_driver::config

#endif // MLDP_WIZARD_ENABLED
