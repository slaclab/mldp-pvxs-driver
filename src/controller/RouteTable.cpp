//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <controller/RouteTable.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mldp_pvxs_driver::controller {

RouteTable RouteTable::build(
    const std::vector<RouteEntry>& routes,
    const std::unordered_set<std::string>& known_readers,
    const std::unordered_set<std::string>& known_writers)
{
    RouteTable rt;
    if (routes.empty()) {
        return rt;  // default-constructed = all_to_all_=true
    }
    rt.all_to_all_ = false;

    for (const auto& [writer_name, reader_names] : routes) {
        if (!known_writers.contains(writer_name)) {
            throw std::runtime_error(
                "Route references unknown writer '" + writer_name + "'");
        }

        WriterRoute wr;
        for (const auto& reader : reader_names) {
            if (reader == "all") {
                wr.accept_all = true;
            } else {
                if (!known_readers.contains(reader)) {
                    throw std::runtime_error(
                        "Route for writer '" + writer_name + "' references unknown reader '" + reader + "'");
                }
                wr.readers.insert(reader);
            }
        }
        rt.table_.emplace(writer_name, std::move(wr));
    }

    return rt;
}

bool RouteTable::isAllToAll() const noexcept
{
    return all_to_all_;
}

bool RouteTable::accepts(const std::string& writer_name,
                         const std::string& reader_name) const noexcept
{
    if (all_to_all_) {
        return true;
    }

    auto it = table_.find(writer_name);
    if (it == table_.end()) {
        return false;
    }

    const auto& wr = it->second;
    if (wr.accept_all) {
        return true;
    }

    return wr.readers.contains(reader_name);
}

std::vector<std::string> RouteTable::orphanReaders(
    const std::unordered_set<std::string>& known_readers) const
{
    std::unordered_set<std::string> mentioned;
    for (const auto& [_, wr] : table_) {
        if (wr.accept_all) {
            return {}; // accept_all means all readers are effectively mentioned
        }
        mentioned.insert(wr.readers.begin(), wr.readers.end());
    }

    std::vector<std::string> orphans;
    for (const auto& reader : known_readers) {
        if (!mentioned.contains(reader)) {
            orphans.push_back(reader);
        }
    }
    std::sort(orphans.begin(), orphans.end());
    return orphans;
}

std::vector<std::string> RouteTable::orphanWriters(
    const std::unordered_set<std::string>& known_writers) const
{
    std::vector<std::string> orphans;
    for (const auto& writer : known_writers) {
        if (!table_.contains(writer)) {
            orphans.push_back(writer);
        }
    }
    std::sort(orphans.begin(), orphans.end());
    return orphans;
}

} // namespace mldp_pvxs_driver::controller
