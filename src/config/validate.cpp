//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <config/validate.h>
#include <set>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {

std::vector<ConfigDiagnostic> validateConfig(const Config& cfg)
{
    std::vector<ConfigDiagnostic> diags;

    auto err = [&](std::string fp, std::string msg)
    {
        diags.push_back({ConfigDiagnostic::Severity::ERROR, std::move(fp), std::move(msg)});
    };
    auto warn = [&](std::string fp, std::string msg)
    {
        diags.push_back({ConfigDiagnostic::Severity::WARN, std::move(fp), std::move(msg)});
    };

    // -------------------------------------------------------------------------
    // Writer block
    // -------------------------------------------------------------------------
    if (!cfg.hasChild("writer"))
    {
        err("writer", "missing required block");
        // Cannot proceed with writer checks — skip straight to readers / metrics
    }
    else
    {
        auto writerVec = cfg.subConfig("writer");
        // subConfig returns a single-element vector for a map child
        const Config& writer = writerVec.empty() ? Config{} : writerVec[0];

        bool hasMldp = writer.hasChild("mldp");
        bool hasHdf5 = writer.hasChild("hdf5");
        bool hasHdf5Merge = writer.hasChild("hdf5-merge");

        if (!hasMldp && !hasHdf5 && !hasHdf5Merge)
        {
            err("writer", "must contain at least one of mldp, hdf5, hdf5-merge");
        }

        // Collect all writer instance names for uniqueness and routing checks
        std::set<std::string>    seenWriterNames;
        std::vector<std::string> allWriterNames;

        // -----------------------------------------------------------------
        // writer.mldp[]
        // -----------------------------------------------------------------
        auto mldpInstances = writer.subConfig("mldp");
        for (std::size_t i = 0; i < mldpInstances.size(); ++i)
        {
            const Config& inst = mldpInstances[i];
            std::string   prefix = "writer.mldp[" + std::to_string(i) + "]";

            // name
            std::string name = inst.get("name", "");
            if (!inst.hasChild("name") || name.empty())
            {
                err(prefix + ".name", "missing or empty");
            }
            else
            {
                if (seenWriterNames.count(name))
                {
                    err("writer", "duplicate instance name '" + name + "'");
                }
                else
                {
                    seenWriterNames.insert(name);
                    allWriterNames.push_back(name);
                }
            }

            // mldp-pool
            if (!inst.hasChild("mldp-pool"))
            {
                err(prefix + ".mldp-pool", "missing required block");
            }
            else
            {
                auto poolVec = inst.subConfig("mldp-pool");
                if (!poolVec.empty())
                {
                    const Config& pool = poolVec[0];
                    std::string   poolPfx = prefix + ".mldp-pool";

                    // provider-name
                    if (!pool.hasChild("provider-name") || pool.get("provider-name", "").empty())
                    {
                        err(poolPfx + ".provider-name", "missing or empty");
                    }

                    // ingestion-url
                    if (!pool.hasChild("ingestion-url") || pool.get("ingestion-url", "").empty())
                    {
                        err(poolPfx + ".ingestion-url", "missing or empty");
                    }

                    // query-url (optional but recommended)
                    if (!pool.hasChild("query-url") || pool.get("query-url", "").empty())
                    {
                        warn(poolPfx + ".query-url", "missing (optional but recommended)");
                    }

                    // min-conn
                    if (!pool.hasChild("min-conn"))
                    {
                        err(poolPfx + ".min-conn", "missing required field");
                    }
                    else
                    {
                        int minConn = pool.getInt("min-conn", 0);
                        if (minConn <= 0)
                        {
                            err(poolPfx + ".min-conn", "must be > 0");
                        }
                    }

                    // max-conn
                    if (!pool.hasChild("max-conn"))
                    {
                        err(poolPfx + ".max-conn", "missing required field");
                    }
                    else
                    {
                        int maxConn = pool.getInt("max-conn", 0);
                        if (maxConn <= 0)
                        {
                            err(poolPfx + ".max-conn", "must be > 0");
                        }
                        else if (pool.hasChild("min-conn"))
                        {
                            int minConn = pool.getInt("min-conn", 0);
                            if (maxConn < minConn)
                            {
                                err(poolPfx + ".max-conn", "must be >= min-conn");
                            }
                        }
                    }
                }
            }

            // thread-pool (optional, must be > 0 if present)
            if (inst.hasChild("thread-pool"))
            {
                int v = inst.getInt("thread-pool", 0);
                if (v <= 0)
                {
                    err(prefix + ".thread-pool", "must be > 0");
                }
            }

            // stream-max-bytes (optional, must be > 0 if present)
            if (inst.hasChild("stream-max-bytes"))
            {
                int v = inst.getInt("stream-max-bytes", 0);
                if (v <= 0)
                {
                    err(prefix + ".stream-max-bytes", "must be > 0");
                }
            }

            // stream-max-age-ms (optional, must be > 0 if present)
            if (inst.hasChild("stream-max-age-ms"))
            {
                int v = inst.getInt("stream-max-age-ms", 0);
                if (v <= 0)
                {
                    err(prefix + ".stream-max-age-ms", "must be > 0");
                }
            }
        }

        // -----------------------------------------------------------------
        // writer.hdf5[] and writer.hdf5-merge[]
        // -----------------------------------------------------------------
        auto validateHdf5Instances = [&](const std::vector<Config>& instances,
                                         const std::string&         tag)
        {
            for (std::size_t i = 0; i < instances.size(); ++i)
            {
                const Config& inst = instances[i];
                std::string   prefix = "writer." + tag + "[" + std::to_string(i) + "]";

                // name
                std::string name = inst.get("name", "");
                if (!inst.hasChild("name") || name.empty())
                {
                    err(prefix + ".name", "missing or empty");
                }
                else
                {
                    if (seenWriterNames.count(name))
                    {
                        err("writer", "duplicate instance name '" + name + "'");
                    }
                    else
                    {
                        seenWriterNames.insert(name);
                        allWriterNames.push_back(name);
                    }
                }

                // base-path
                if (!inst.hasChild("base-path") || inst.get("base-path", "").empty())
                {
                    err(prefix + ".base-path", "missing or empty");
                }

                // compression-level [0,9]
                if (inst.hasChild("compression-level"))
                {
                    int v = inst.getInt("compression-level", 0);
                    if (v < 0 || v > 9)
                    {
                        err(prefix + ".compression-level", "must be in [0,9]");
                    }
                }

                // max-file-age-s (optional, nonzero must be > 0)
                if (inst.hasChild("max-file-age-s"))
                {
                    int v = inst.getInt("max-file-age-s", 0);
                    if (v != 0 && v <= 0)
                    {
                        err(prefix + ".max-file-age-s", "must be > 0");
                    }
                }

                // max-file-size-mb (optional, nonzero must be > 0)
                if (inst.hasChild("max-file-size-mb"))
                {
                    int v = inst.getInt("max-file-size-mb", 0);
                    if (v != 0 && v <= 0)
                    {
                        err(prefix + ".max-file-size-mb", "must be > 0");
                    }
                }

                // flush-interval-ms (optional, nonzero must be > 0)
                if (inst.hasChild("flush-interval-ms"))
                {
                    int v = inst.getInt("flush-interval-ms", 0);
                    if (v != 0 && v <= 0)
                    {
                        err(prefix + ".flush-interval-ms", "must be > 0");
                    }
                }
            }
        };

        validateHdf5Instances(writer.subConfig("hdf5"), "hdf5");
        validateHdf5Instances(writer.subConfig("hdf5-merge"), "hdf5-merge");

        // -------------------------------------------------------------------------
        // Reader block
        // -------------------------------------------------------------------------
        if (!cfg.hasChild("reader"))
        {
            err("reader", "missing required block");
        }

        std::set<std::string>    seenReaderNames;
        std::vector<std::string> allReaderNames;

        auto readerSequence = cfg.subConfig("reader");
        for (std::size_t N = 0; N < readerSequence.size(); ++N)
        {
            const Config& readerEntry = readerSequence[N];
            std::string   readerPfx = "reader[" + std::to_string(N) + "]";

            // Each entry is a map whose keys are reader-type names.
            // We check for known types: epics-pvxs, epics-base, epics-archiver.

            auto validateEpicsMonitor = [&](const std::vector<Config>& instances,
                                            const std::string&         rtype)
            {
                for (std::size_t i = 0; i < instances.size(); ++i)
                {
                    const Config& inst = instances[i];
                    std::string   pfx = readerPfx + "." + rtype + "[" + std::to_string(i) + "]";

                    // name
                    std::string name = inst.get("name", "");
                    if (!inst.hasChild("name") || name.empty())
                    {
                        err(pfx + ".name", "missing or empty");
                    }
                    else
                    {
                        if (seenReaderNames.count(name))
                        {
                            err("reader", "duplicate instance name '" + name + "'");
                        }
                        else
                        {
                            seenReaderNames.insert(name);
                            allReaderNames.push_back(name);
                        }
                    }

                    // pvs — if present must be a sequence
                    if (inst.hasChild("pvs"))
                    {
                        if (!inst.isSequence("pvs"))
                        {
                            err(pfx + ".pvs", "must be a sequence");
                        }
                        else
                        {
                            auto pvList = inst.subConfig("pvs");
                            for (std::size_t j = 0; j < pvList.size(); ++j)
                            {
                                const Config& pv = pvList[j];
                                std::string   pvPfx = pfx + ".pvs[" + std::to_string(j) + "]";
                                std::string   pvName = pv.get("name", "");
                                if (!pv.hasChild("name") || pvName.empty())
                                {
                                    err(pvPfx + ".name", "missing or empty");
                                }
                            }
                        }
                    }
                }
            };

            if (readerEntry.hasChild("epics-pvxs"))
            {
                validateEpicsMonitor(readerEntry.subConfig("epics-pvxs"), "epics-pvxs");
            }
            if (readerEntry.hasChild("epics-base"))
            {
                validateEpicsMonitor(readerEntry.subConfig("epics-base"), "epics-base");
            }

            // epics-archiver
            if (readerEntry.hasChild("epics-archiver"))
            {
                auto archiverInstances = readerEntry.subConfig("epics-archiver");
                for (std::size_t i = 0; i < archiverInstances.size(); ++i)
                {
                    const Config& inst = archiverInstances[i];
                    std::string   pfx = readerPfx + ".epics-archiver[" + std::to_string(i) + "]";

                    // name
                    std::string name = inst.get("name", "");
                    if (!inst.hasChild("name") || name.empty())
                    {
                        err(pfx + ".name", "missing or empty");
                    }
                    else
                    {
                        if (seenReaderNames.count(name))
                        {
                            err("reader", "duplicate instance name '" + name + "'");
                        }
                        else
                        {
                            seenReaderNames.insert(name);
                            allReaderNames.push_back(name);
                        }
                    }

                    // hostname
                    if (!inst.hasChild("hostname") || inst.get("hostname", "").empty())
                    {
                        err(pfx + ".hostname", "missing or empty");
                    }

                    // mode
                    std::string mode;
                    if (inst.hasChild("mode"))
                    {
                        mode = inst.get("mode", "");
                        if (mode != "historical_once" && mode != "periodic_tail")
                        {
                            err(pfx + ".mode", "must be historical_once or periodic_tail");
                            mode.clear(); // inhibit dependent checks
                        }
                    }

                    if (mode == "historical_once")
                    {
                        std::string startDate = inst.get("start-date", "");
                        if (!inst.hasChild("start-date") || startDate.empty())
                        {
                            err(pfx + ".start-date", "required for mode=historical_once");
                        }
                    }

                    if (mode == "periodic_tail")
                    {
                        if (!inst.hasChild("poll-interval-sec") ||
                            inst.getInt("poll-interval-sec", 0) == 0)
                        {
                            err(pfx + ".poll-interval-sec", "required for mode=periodic_tail");
                        }
                    }

                    // connect-timeout-sec (optional, nonzero must be > 0)
                    if (inst.hasChild("connect-timeout-sec"))
                    {
                        int v = inst.getInt("connect-timeout-sec", 0);
                        if (v != 0 && v <= 0)
                        {
                            err(pfx + ".connect-timeout-sec", "must be > 0");
                        }
                    }

                    // total-timeout-sec must be >= connect-timeout-sec when both > 0
                    if (inst.hasChild("total-timeout-sec"))
                    {
                        int total = inst.getInt("total-timeout-sec", 0);
                        int connect = inst.getInt("connect-timeout-sec", 0);
                        if (total > 0 && total < connect)
                        {
                            err(pfx + ".total-timeout-sec", "must be >= connect-timeout-sec");
                        }
                    }

                    // pvs — if present must be a sequence
                    if (inst.hasChild("pvs"))
                    {
                        if (!inst.isSequence("pvs"))
                        {
                            err(pfx + ".pvs", "must be a sequence");
                        }
                    }
                }
            }
        }

        // -------------------------------------------------------------------------
        // Metrics block (optional)
        // -------------------------------------------------------------------------
        if (cfg.hasChild("metrics"))
        {
            auto metricsVec = cfg.subConfig("metrics");
            if (!metricsVec.empty())
            {
                const Config& metrics = metricsVec[0];

                if (!metrics.hasChild("endpoint") || metrics.get("endpoint", "").empty())
                {
                    err("metrics.endpoint", "missing or empty");
                }

                if (metrics.hasChild("scan-interval-seconds"))
                {
                    int v = metrics.getInt("scan-interval-seconds", 0);
                    if (v < 1)
                    {
                        err("metrics.scan-interval-seconds", "must be >= 1");
                    }
                }
            }
        }

        // -------------------------------------------------------------------------
        // Routing block (optional)
        // -------------------------------------------------------------------------
        if (cfg.hasChild("routing"))
        {
            auto routingVec = cfg.subConfig("routing");
            if (!routingVec.empty())
            {
                const Config& routing = routingVec[0];

                // Build lookup sets
                std::set<std::string> writerNameSet(allWriterNames.begin(), allWriterNames.end());
                std::set<std::string> readerNameSet(allReaderNames.begin(), allReaderNames.end());

                // Iterate routing entries via raw node children
                // Since we only have the Config wrapper API we use subConfig-based
                // iteration: for each writer name key in allWriterNames check presence,
                // then walk 'from' entries. We also need to detect unknown writer keys
                // that appear in routing. We do this by checking via raw() children.
                // However, to avoid using ryml headers directly we use a two-pass
                // approach: first check all known writers declared in routing, then
                // warn about any writer-side reference issues.
                //
                // The routing block is a map of writerName -> { from: [...] }.
                // We iterate by checking each key found under routing against
                // our declared name sets. Because Config doesn't expose a
                // key-enumeration API we rely on the raw node via the existing
                // operator>> to extract keys.
                std::map<std::string, std::string> routingMap;
                // Use the map extractor to get the top-level routing keys.
                // The values aren't meaningful here; we just want the keys.
                // Re-use subConfig per known writer to walk from-lists, and
                // detect unknown writers by checking all allWriterNames against
                // hasChild — for unknown writers present in routing we need
                // to walk the raw node.  Since ryml is available transitively
                // through Config.h we access it via raw().

                const auto& rawRouting = routing.raw();
                if (rawRouting.readable() && rawRouting.is_map())
                {
                    for (const auto child : rawRouting.children())
                    {
                        if (!child.has_key())
                            continue;

                        // Extract writer name key
                        const auto  keyView = child.key();
                        std::string writerName{keyView.str, keyView.len};

                        if (!writerNameSet.count(writerName))
                        {
                            warn("routing." + writerName, "unknown writer name");
                        }

                        // Check 'from' entries
                        if (child.has_child("from"))
                        {
                            auto fromNode = child["from"];
                            if (fromNode.is_seq())
                            {
                                std::size_t j = 0;
                                for (const auto fromEntry : fromNode.children())
                                {
                                    std::string fromName;
                                    if (fromEntry.has_val())
                                    {
                                        fromEntry >> fromName;
                                    }
                                    if (fromName != "all" && !readerNameSet.count(fromName))
                                    {
                                        warn("routing." + writerName + ".from[" +
                                                 std::to_string(j) + "]",
                                             "unknown reader name '" + fromName + "'");
                                    }
                                    ++j;
                                }
                            }
                        }
                    }
                }
            }
        }
    } // end writer block

    return diags;
}

} // namespace mldp_pvxs_driver::config
