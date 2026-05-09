//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <config/edit.h>
#include <config/Config.h>
#include <config/validate.h>
#include <config/wizard.h>
#include "wizard_internal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// I/O helpers
// ─────────────────────────────────────────────────────────────────────────────

bool probeFile(const std::string& path)
{
    std::ifstream f{path};
    if (!f) {
        std::cerr << "ERROR  cannot open '" << path << "'\n";
        return false;
    }
    f.seekg(0, std::ios::end);
    if (f.tellg() == 0) {
        std::cerr << "ERROR  '" << path << "' is empty\n";
        return false;
    }
    return true;
}

bool loadState(const std::string& path, WizardState& st)
{
    if (!probeFile(path)) return false;
    try {
        wizard_internal::loadFromConfig(path, st);
    } catch (const std::exception& e) {
        std::cerr << "ERROR  failed to parse '" << path << "': " << e.what() << "\n";
        return false;
    }
    return true;
}

bool writeConfig(const std::string& path, const WizardState& st,
                 bool dry_run, bool no_backup)
{
    const std::string yaml = wizard_internal::generateYaml(st);

    if (dry_run) {
        std::cout << yaml;
        return true;
    }

    if (!no_backup) {
        const std::string bak = path + ".bak";
        try {
            std::filesystem::copy_file(path, bak,
                std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            std::cerr << "WARN   could not write backup '" << bak << "': " << e.what() << "\n";
        }
    }

    std::ofstream out{path};
    if (!out) {
        std::cerr << "ERROR  cannot write '" << path << "'\n";
        return false;
    }
    out << yaml;
    return true;
}

bool runValidateGate(const WizardState& st, const std::string& context)
{
    const std::string yaml = wizard_internal::generateYaml(st);
    auto tree = std::make_shared<ryml::Tree>(ryml::parse_in_arena(c4::to_csubstr(yaml)));
    Config cfg(tree);
    const auto diags = validateConfig(cfg);

    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == ConfigDiagnostic::Severity::ERROR) {
            std::cerr << "ERROR  " << d.field_path << "  " << d.message << "\n";
            has_error = true;
        } else {
            std::cerr << "WARN   " << d.field_path << "  " << d.message << "\n";
        }
    }
    if (has_error) {
        std::cerr << "FAIL  " << context << " — result would be invalid\n";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// list formatting
// ─────────────────────────────────────────────────────────────────────────────

void printList(const WizardState& st, const std::string& path)
{
    std::cout << "File: " << path << "\n\n";

    // Writers
    std::cout << "Writers\n";
    for (const auto& w : st.mldp_writers) {
        std::cout << "  " << std::left << std::setw(20) << w.name
                  << std::setw(12) << "mldp"
                  << "thread-pool=" << w.thread_pool
                  << "  ingestion-url=" << w.ingestion_url
                  << "\n";
    }
    for (const auto& w : st.hdf5_writers) {
        const std::string tag = w.is_merge ? "hdf5-merge" : "hdf5";
        std::cout << "  " << std::left << std::setw(20) << w.name
                  << std::setw(12) << tag
                  << "base-path=" << w.base_path
                  << "  compression=" << w.compression_level
                  << "\n";
    }
    if (st.mldp_writers.empty() && st.hdf5_writers.empty())
        std::cout << "  (none)\n";

    // Readers
    std::cout << "\nReaders\n";
    for (const auto& r : st.readers) {
        std::cout << "  " << std::left << std::setw(20) << r.name
                  << std::setw(16) << r.reader_type
                  << r.pvs.size() << " PVs"
                  << "\n";
    }
    if (st.readers.empty())
        std::cout << "  (none)\n";

    // Routing
    if (st.routing_all_to_all || st.routing.empty()) {
        std::cout << "\nRouting      all-to-all\n";
    } else {
        std::cout << "\nRouting\n";
        for (const auto& re : st.routing) {
            std::cout << "  " << re.writer_name << "  <-";
            if (re.from_readers.empty()) {
                std::cout << " all";
            } else {
                for (const auto& r : re.from_readers) std::cout << " " << r;
            }
            for (const auto& g : re.include_globs) std::cout << "  [include: " << g << "]";
            for (const auto& g : re.exclude_globs) std::cout << "  [exclude: " << g << "]";
            std::cout << "\n";
        }
    }

    // Metrics
    if (st.metrics_enabled) {
        std::cout << "\nMetrics      " << st.metrics_endpoint
                  << "  scan-interval=" << st.metrics_interval << "s\n";
    } else {
        std::cout << "\nMetrics      disabled\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// split helpers
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string> splitComma(const std::string& s)
{
    if (s.empty()) return {};
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// runList
// ─────────────────────────────────────────────────────────────────────────────

int runList(const EditListOptions& opts)
{
    WizardState st;
    if (!loadState(opts.path, st)) return 1;
    printList(st, opts.path);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// runRemove
// ─────────────────────────────────────────────────────────────────────────────

int runRemove(const EditRemoveOptions& opts)
{
    WizardState st;
    if (!loadState(opts.path, st)) return 1;

    const std::string& kind = opts.kind;
    const std::string& name = opts.name;

    if (kind == "writer") {
        bool found = false;

        auto& mv = st.mldp_writers;
        auto mit = std::find_if(mv.begin(), mv.end(),
            [&](const MldpWriterConfig& w){ return w.name == name; });
        if (mit != mv.end()) {
            const int total = static_cast<int>(mv.size() + st.hdf5_writers.size());
            if (total <= 1) {
                std::cerr << "ERROR  cannot remove last writer — config would be invalid\n";
                return 1;
            }
            mv.erase(mit);
            found = true;
        }

        if (!found) {
            auto& hv = st.hdf5_writers;
            auto hit = std::find_if(hv.begin(), hv.end(),
                [&](const Hdf5WriterConfig& w){ return w.name == name; });
            if (hit != hv.end()) {
                const int total = static_cast<int>(mv.size() + hv.size());
                if (total <= 1) {
                    std::cerr << "ERROR  cannot remove last writer — config would be invalid\n";
                    return 1;
                }
                hv.erase(hit);
                found = true;
            }
        }

        if (!found) {
            std::cerr << "ERROR  writer '" << name << "' not found in " << opts.path << "\n";
            return 1;
        }

        // Clean up routing entries for this writer
        auto& rv = st.routing;
        rv.erase(std::remove_if(rv.begin(), rv.end(),
            [&](const RoutingEntry& r){ return r.writer_name == name; }), rv.end());

    } else if (kind == "reader") {
        auto& rv = st.readers;
        auto it = std::find_if(rv.begin(), rv.end(),
            [&](const EpicsReaderConfig& r){ return r.name == name; });
        if (it == rv.end()) {
            std::cerr << "ERROR  reader '" << name << "' not found in " << opts.path << "\n";
            return 1;
        }
        rv.erase(it);

        // Clean from_readers lists; warn if entry becomes empty
        for (auto& re : st.routing) {
            auto& fr = re.from_readers;
            auto fit = std::find(fr.begin(), fr.end(), name);
            if (fit != fr.end()) {
                fr.erase(fit);
                if (fr.empty())
                    std::cerr << "WARN   routing entry for '" << re.writer_name
                              << "' now has no explicit from-readers\n";
            }
        }

    } else if (kind == "routing") {
        auto& rv = st.routing;
        auto it = std::find_if(rv.begin(), rv.end(),
            [&](const RoutingEntry& r){ return r.writer_name == name; });
        if (it == rv.end()) {
            std::cerr << "ERROR  no routing entry for writer '" << name << "' in " << opts.path << "\n";
            return 1;
        }
        rv.erase(it);
        if (st.routing.empty())
            st.routing_all_to_all = true;

    } else {
        std::cerr << "ERROR  unknown kind '" << kind << "' — use writer, reader, or routing\n";
        return 1;
    }

    // Validate result (warnings OK, errors block)
    if (!runValidateGate(st, opts.path)) return 1;

    if (!writeConfig(opts.path, st, opts.dry_run, opts.no_backup)) return 1;

    if (!opts.dry_run) {
        std::cout << "Removed " << kind << " '" << name << "' from " << opts.path << "\n";
        if (!opts.no_backup)
            std::cout << "Backup: " << opts.path << ".bak\n";
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// runAdd
// ─────────────────────────────────────────────────────────────────────────────

int runAdd(const EditAddOptions& opts)
{
    WizardState st;
    if (!loadState(opts.path, st)) return 1;

    const std::string& kind = opts.kind;

    // ── writer ────────────────────────────────────────────────────────────────
    if (kind == "writer") {
        if (opts.name.empty()) {
            std::cerr << "ERROR  --name required for non-interactive add\n";
            return 1;
        }
        if (opts.type.empty()) {
            std::cerr << "ERROR  --type required (mldp, hdf5, hdf5-merge)\n";
            return 1;
        }

        // Uniqueness check
        for (const auto& w : st.mldp_writers)
            if (w.name == opts.name) {
                std::cerr << "ERROR  writer '" << opts.name << "' already exists\n";
                return 1;
            }
        for (const auto& w : st.hdf5_writers)
            if (w.name == opts.name) {
                std::cerr << "ERROR  writer '" << opts.name << "' already exists\n";
                return 1;
            }

        if (opts.type == "mldp") {
            if (opts.ingestion_url.empty()) {
                std::cerr << "ERROR  --ingestion-url required for mldp writer\n";
                return 1;
            }
            if (opts.provider_name.empty()) {
                std::cerr << "ERROR  --provider-name required for mldp writer\n";
                return 1;
            }
            MldpWriterConfig w;
            w.name            = opts.name;
            w.thread_pool     = opts.thread_pool.empty()     ? "1"   : opts.thread_pool;
            w.ingestion_url   = opts.ingestion_url;
            w.provider_name   = opts.provider_name;
            w.query_url       = opts.query_url;
            w.min_conn        = opts.min_conn.empty()        ? "1"   : opts.min_conn;
            w.max_conn        = opts.max_conn.empty()        ? "4"   : opts.max_conn;
            w.creds_type      = opts.credentials.empty()     ? "ssl" : opts.credentials;
            w.stream_max_bytes  = opts.stream_max_bytes.empty()  ? "2097152" : opts.stream_max_bytes;
            w.stream_max_age_ms = opts.stream_max_age_ms.empty() ? "200"     : opts.stream_max_age_ms;
            st.mldp_writers.push_back(std::move(w));

        } else if (opts.type == "hdf5" || opts.type == "hdf5-merge") {
            if (opts.base_path.empty()) {
                std::cerr << "ERROR  --base-path required for " << opts.type << " writer\n";
                return 1;
            }
            Hdf5WriterConfig w;
            w.is_merge          = (opts.type == "hdf5-merge");
            w.name              = opts.name;
            w.base_path         = opts.base_path;
            w.compression_level = opts.compression_level.empty() ? "0"    : opts.compression_level;
            w.max_file_age_s    = opts.max_file_age_s.empty()    ? "3600" : opts.max_file_age_s;
            w.max_file_size_mb  = opts.max_file_size_mb.empty()  ? "512"  : opts.max_file_size_mb;
            st.hdf5_writers.push_back(std::move(w));

        } else {
            std::cerr << "ERROR  unknown writer type '" << opts.type << "' — use mldp, hdf5, hdf5-merge\n";
            return 1;
        }

    // ── reader ────────────────────────────────────────────────────────────────
    } else if (kind == "reader") {
        if (opts.name.empty()) {
            std::cerr << "ERROR  --name required for non-interactive add\n";
            return 1;
        }
        if (opts.type.empty()) {
            std::cerr << "ERROR  --type required (epics-pvxs, epics-base, epics-archiver)\n";
            return 1;
        }

        for (const auto& r : st.readers)
            if (r.name == opts.name) {
                std::cerr << "ERROR  reader '" << opts.name << "' already exists\n";
                return 1;
            }

        EpicsReaderConfig r;
        r.name        = opts.name;
        r.reader_type = opts.type;
        r.thread_pool       = opts.reader_thread_pool.empty() ? "2"  : opts.reader_thread_pool;
        r.column_batch_size = opts.column_batch_size.empty()  ? "50" : opts.column_batch_size;

        if (opts.type == "epics-archiver") {
            if (opts.hostname.empty()) {
                std::cerr << "ERROR  --hostname required for epics-archiver reader\n";
                return 1;
            }
            r.hostname            = opts.hostname;
            r.mode                = opts.mode.empty()               ? "historical_once" : opts.mode;
            r.start_date          = opts.start_date;
            r.end_date            = opts.end_date;
            r.poll_interval_sec   = opts.poll_interval_sec;
            r.connect_timeout_sec = opts.connect_timeout_sec.empty() ? "30"  : opts.connect_timeout_sec;
            r.total_timeout_sec   = opts.total_timeout_sec.empty()   ? "300" : opts.total_timeout_sec;

            if (r.mode == "historical_once" && r.start_date.empty()) {
                std::cerr << "ERROR  --start-date required for historical_once mode\n";
                return 1;
            }
            if (r.mode == "periodic_tail" && r.poll_interval_sec.empty()) {
                std::cerr << "ERROR  --poll-interval-sec required for periodic_tail mode\n";
                return 1;
            }
        } else if (opts.type != "epics-pvxs" && opts.type != "epics-base") {
            std::cerr << "ERROR  unknown reader type '" << opts.type
                      << "' — use epics-pvxs, epics-base, epics-archiver\n";
            return 1;
        }

        // Parse PV list
        if (!opts.pvs.empty()) {
            for (const auto& pv_name : splitComma(opts.pvs)) {
                PvEntry pv;
                pv.name        = pv_name;
                pv.option_type = "none";
                r.pvs.push_back(std::move(pv));
            }
        }

        st.readers.push_back(std::move(r));

    // ── routing ───────────────────────────────────────────────────────────────
    } else if (kind == "routing") {
        const std::string& writer_name = opts.writer_name.empty() ? opts.name : opts.writer_name;
        if (writer_name.empty()) {
            std::cerr << "ERROR  --writer required for routing add\n";
            return 1;
        }

        // Verify writer exists
        bool writer_found = false;
        for (const auto& w : st.mldp_writers)
            if (w.name == writer_name) { writer_found = true; break; }
        if (!writer_found)
            for (const auto& w : st.hdf5_writers)
                if (w.name == writer_name) { writer_found = true; break; }
        if (!writer_found) {
            std::cerr << "ERROR  writer '" << writer_name << "' not found in config\n";
            return 1;
        }

        // Check for existing routing entry
        auto it = std::find_if(st.routing.begin(), st.routing.end(),
            [&](const RoutingEntry& r){ return r.writer_name == writer_name; });

        std::vector<std::string> from_readers = splitComma(opts.from);

        if (it != st.routing.end() && !opts.replace) {
            // Merge
            for (const auto& r : from_readers) {
                if (std::find(it->from_readers.begin(), it->from_readers.end(), r) == it->from_readers.end())
                    it->from_readers.push_back(r);
            }
            for (const auto& g : opts.include_globs) {
                if (std::find(it->include_globs.begin(), it->include_globs.end(), g) == it->include_globs.end())
                    it->include_globs.push_back(g);
            }
            for (const auto& g : opts.exclude_globs) {
                if (std::find(it->exclude_globs.begin(), it->exclude_globs.end(), g) == it->exclude_globs.end())
                    it->exclude_globs.push_back(g);
            }
        } else if (it != st.routing.end() && opts.replace) {
            it->from_readers   = std::move(from_readers);
            it->include_globs  = opts.include_globs;
            it->exclude_globs  = opts.exclude_globs;
        } else {
            RoutingEntry re;
            re.writer_name    = writer_name;
            re.from_readers   = std::move(from_readers);
            re.include_globs  = opts.include_globs;
            re.exclude_globs  = opts.exclude_globs;
            st.routing.push_back(std::move(re));
        }

        // Switching from implicit all-to-all to explicit routing
        if (st.routing_all_to_all) {
            st.routing_all_to_all = false;
            std::cerr << "WARN   switching from all-to-all to explicit routing; "
                         "writers not listed in routing block will receive nothing\n";
        }

    } else {
        std::cerr << "ERROR  unknown kind '" << kind << "' — use writer, reader, or routing\n";
        return 1;
    }

    if (!runValidateGate(st, opts.path)) return 1;

    if (!writeConfig(opts.path, st, opts.dry_run, opts.no_backup)) return 1;

    if (!opts.dry_run) {
        const std::string display_name = (kind == "routing")
            ? (opts.writer_name.empty() ? opts.name : opts.writer_name)
            : opts.name;
        std::cout << "Added " << kind << " '" << display_name;
        if (!opts.type.empty()) std::cout << "' (" << opts.type << ")";
        else                    std::cout << "'";
        std::cout << " to " << opts.path << "\n";
        if (!opts.no_backup)
            std::cout << "Backup: " << opts.path << ".bak\n";
    }
    return 0;
}

} // namespace mldp_pvxs_driver::config
