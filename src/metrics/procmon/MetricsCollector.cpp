//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "procmon/MetricsCollector.hpp"
#include "procmon/ProcReader.hpp"
#include <algorithm>
#include <optional>

namespace procmon {

MetricsCollector::MetricsCollector(pid_t pid) : pid_(pid) {}

void MetricsCollector::register_handler(std::shared_ptr<MetricsHandler> handler) {
    if (handler) {
        handlers_.push_back(handler);
    }
}

bool MetricsCollector::is_process_alive() const {
    return ProcReader::process_exists(pid_);
}

Result<MetricsSnapshot> MetricsCollector::collect() {
    // Check if process exists
    if (!is_process_alive()) {
        std::string error = "Process " + std::to_string(pid_) + " does not exist";
        notify_error(error);
        return Result<MetricsSnapshot>::error(error);
    }
    
    // Create snapshot with timestamp
    MetricsSnapshot snapshot;
    snapshot.pid = pid_;
    snapshot.timestamp = std::chrono::system_clock::now();
    
    // Read comm first (used by stat as well)
    auto comm_result = ProcReader::read_comm(pid_);
    if (comm_result) {
        snapshot.comm = *comm_result;
    }
    
    // Read stat file
    if (auto result = ProcReader::read_stat(pid_, snapshot); !result) {
        notify_error(result.error());
        return Result<MetricsSnapshot>::error(result.error());
    }
    
    // Read status file
    if (auto result = ProcReader::read_status(pid_, snapshot); !result) {
        notify_error(result.error());
        return Result<MetricsSnapshot>::error(result.error());
    }
    
    // Read I/O stats (may fail due to permissions, non-critical)
    ProcReader::read_io(pid_, snapshot);
    
    // Count file descriptors (may fail due to permissions, non-critical)
    if (auto fd_count = ProcReader::count_fds(pid_); fd_count) {
        snapshot.num_fds = *fd_count;
    } else {
        snapshot.num_fds = std::nullopt;
    }
    
    // Notify handlers
    notify_handlers(snapshot);
    
    return snapshot;
}

void MetricsCollector::notify_handlers(const MetricsSnapshot& snapshot) {
    for (const auto& handler : handlers_) {
        if (handler) {
            handler->onMetricsSnapshot(snapshot);
        }
    }
}

void MetricsCollector::notify_error(const std::string& error_message) {
    for (const auto& handler : handlers_) {
        if (handler) {
            handler->onError(error_message);
        }
    }
}

} // namespace procmon
