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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mldp_pvxs_driver::controller {

class RouteTable
{
public:
    /// Route entry: writer_name → list of accepted reader names
    using RouteEntry = std::pair<std::string, std::vector<std::string>>;

    RouteTable() = default;

    /// Build from parsed config. Validates all names exist.
    /// @throws std::runtime_error on unknown reader/writer names.
    static RouteTable build(
        const std::vector<RouteEntry>& routes,
        const std::unordered_set<std::string>& known_readers,
        const std::unordered_set<std::string>& known_writers);

    /// True when no routing configured → all-to-all.
    bool isAllToAll() const noexcept;

    /// Check if writer should receive batches from given reader. O(1) average.
    bool accepts(const std::string& writer_name,
                 const std::string& reader_name) const noexcept;

    /// Return reader names not mentioned in any route.
    std::vector<std::string> orphanReaders(
        const std::unordered_set<std::string>& known_readers) const;

    /// Return writer names not mentioned in any route.
    std::vector<std::string> orphanWriters(
        const std::unordered_set<std::string>& known_writers) const;

private:
    /// writer_name → set of accepted reader names. "all" sentinel stored as special empty set with all_accept flag.
    struct WriterRoute {
        std::unordered_set<std::string> readers;
        bool accept_all{false};
    };
    std::unordered_map<std::string, WriterRoute> table_;
    bool all_to_all_{true};
};

} // namespace mldp_pvxs_driver::controller
