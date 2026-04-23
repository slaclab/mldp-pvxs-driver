//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "procmon/ProcReader.hpp"
#include <cstring>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <vector>
#else
#include <fstream>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>
#include <dirent.h>
#endif

namespace procmon {

#if !defined(__APPLE__)
Result<std::string> ProcReader::read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return Result<std::string>::error("Failed to open file: " + path);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
#endif

#if !defined(__APPLE__)
std::optional<std::string> ProcReader::parse_key_value(const std::string& line, const std::string& key) {
    if (line.find(key) == 0) {
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string value = line.substr(pos + 1);
            // Trim leading whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            // Remove trailing units like " kB"
            auto space_pos = value.find(' ');
            if (space_pos != std::string::npos) {
                value = value.substr(0, space_pos);
            }
            return value;
        }
    }
    return std::nullopt;
}
#endif

#if !defined(__APPLE__)
uint64_t ProcReader::to_uint64(const std::string& str) noexcept {
    try {
        return std::stoull(str);
    } catch (...) {
        return 0;
    }
}

int64_t ProcReader::to_int64(const std::string& str) noexcept {
    try {
        return std::stoll(str);
    } catch (...) {
        return 0;
    }
}
#endif

#if defined(__APPLE__)
namespace {
std::optional<uint64_t> ns_to_ticks(uint64_t ns) {
    const long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>((ns * static_cast<uint64_t>(ticks_per_sec)) / 1000000000ULL);
}
}

bool ProcReader::process_exists(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}
#else
bool ProcReader::process_exists(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid);
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
#endif

#if defined(__APPLE__)
Result<std::string> ProcReader::read_comm(pid_t pid) {
    struct proc_bsdinfo bsd_info;
    const int bytes = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd_info, sizeof(bsd_info));
    if (bytes <= 0) {
        return Result<std::string>::error("Failed to read process command name");
    }

    std::string comm(bsd_info.pbi_comm);
    return comm;
}
#else
Result<std::string> ProcReader::read_comm(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    auto result = read_file(path);
    if (!result) {
        return Result<std::string>::error(result.error());
    }
    
    // Remove trailing newline
    std::string comm = *result;
    if (!comm.empty() && comm.back() == '\n') {
        comm.pop_back();
    }
    return comm;
}
#endif

#if defined(__APPLE__)
Result<void> ProcReader::read_stat(pid_t pid, MetricsSnapshot& snapshot) {
    struct proc_taskinfo task_info;
    const int bytes = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task_info, sizeof(task_info));
    if (bytes <= 0) {
        return Result<void>::error("Failed to read process task info");
    }

    if (auto ticks = ns_to_ticks(task_info.pti_total_user)) {
        snapshot.utime = *ticks;
    } else {
        snapshot.utime = std::nullopt;
    }

    if (auto ticks = ns_to_ticks(task_info.pti_total_system)) {
        snapshot.stime = *ticks;
    } else {
        snapshot.stime = std::nullopt;
    }

    snapshot.cutime = std::nullopt;
    snapshot.cstime = std::nullopt;
    snapshot.num_threads = static_cast<int64_t>(task_info.pti_threadnum);

    struct proc_bsdinfo bsd_info;
    const int bsd_bytes = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd_info, sizeof(bsd_info));
    if (bsd_bytes > 0) {
        snapshot.priority = std::nullopt;
        snapshot.nice = static_cast<int64_t>(bsd_info.pbi_nice);
        snapshot.comm = std::string(bsd_info.pbi_comm);
    } else {
        snapshot.priority = std::nullopt;
        snapshot.nice = std::nullopt;
    }

    snapshot.pid = pid;
    return Result<void>();
}
#else
Result<void> ProcReader::read_stat(pid_t pid, MetricsSnapshot& snapshot) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    auto result = read_file(path);
    if (!result) {
        return Result<void>::error(result.error());
    }
    
    const std::string& content = *result;
    
    // Parse /proc/[pid]/stat - this is tricky because comm can contain spaces and parentheses
    // Format: pid (comm) state ...
    auto first_paren = content.find('(');
    auto last_paren = content.rfind(')');
    
    if (first_paren == std::string::npos || last_paren == std::string::npos) {
        return Result<void>::error("Invalid stat file format");
    }
    
    // Extract comm (between parentheses)
    snapshot.comm = content.substr(first_paren + 1, last_paren - first_paren - 1);
    
    // Parse fields after comm
    std::istringstream iss(content.substr(last_paren + 2));
    
    // Fields (0-indexed from after comm):
    // 0: state, 1: ppid, 2: pgrp, 3: session, 4: tty_nr, 5: tpgid, 6: flags
    // 7: minflt, 8: cminflt, 9: majflt, 10: cmajflt
    // 11: utime, 12: stime, 13: cutime, 14: cstime
    // 15: priority, 16: nice, 17: num_threads
    
    std::string state, dummy;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned long flags, minflt, cminflt, majflt, cmajflt;
    
    uint64_t utime = 0;
    uint64_t stime = 0;
    uint64_t cutime = 0;
    uint64_t cstime = 0;
    int64_t priority = 0;
    int64_t nice = 0;
    int64_t num_threads = 0;

    iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags
        >> minflt >> cminflt >> majflt >> cmajflt
        >> utime >> stime >> cutime >> cstime
        >> priority >> nice >> num_threads;
    
    if (iss.fail()) {
        return Result<void>::error("Failed to parse stat file");
    }
    
    snapshot.utime = utime;
    snapshot.stime = stime;
    snapshot.cutime = cutime;
    snapshot.cstime = cstime;
    snapshot.priority = priority;
    snapshot.nice = nice;
    snapshot.num_threads = num_threads;
    snapshot.pid = pid;
    return Result<void>();
}
#endif

#if defined(__APPLE__)
Result<void> ProcReader::read_status(pid_t pid, MetricsSnapshot& snapshot) {
    struct proc_taskinfo task_info;
    const int bytes = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task_info, sizeof(task_info));
    if (bytes <= 0) {
        return Result<void>::error("Failed to read process memory info");
    }

    snapshot.vm_size = task_info.pti_virtual_size;
    snapshot.vm_rss = task_info.pti_resident_size;
    snapshot.vm_peak = std::nullopt;
    snapshot.rss_anon = std::nullopt;
    snapshot.rss_file = std::nullopt;
    snapshot.rss_shmem = std::nullopt;

    snapshot.voluntary_ctxt_switches = std::nullopt;
    snapshot.nonvoluntary_ctxt_switches = std::nullopt;

    return Result<void>();
}
#else
Result<void> ProcReader::read_status(pid_t pid, MetricsSnapshot& snapshot) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    auto result = read_file(path);
    if (!result) {
        return Result<void>::error(result.error());
    }
    
    const std::string& content = *result;
    std::istringstream iss(content);
    std::string line;
    
    // Initialize values
    snapshot.vm_size = std::nullopt;
    snapshot.vm_rss = std::nullopt;
    snapshot.vm_peak = std::nullopt;
    snapshot.rss_anon = std::nullopt;
    snapshot.rss_file = std::nullopt;
    snapshot.rss_shmem = std::nullopt;
    snapshot.voluntary_ctxt_switches = std::nullopt;
    snapshot.nonvoluntary_ctxt_switches = std::nullopt;
    
    while (std::getline(iss, line)) {
        if (auto val = parse_key_value(line, "VmSize")) {
            snapshot.vm_size = to_uint64(*val) * 1024; // Convert kB to bytes
        } else if (auto val = parse_key_value(line, "VmRSS")) {
            snapshot.vm_rss = to_uint64(*val) * 1024;
        } else if (auto val = parse_key_value(line, "VmPeak")) {
            snapshot.vm_peak = to_uint64(*val) * 1024;
        } else if (auto val = parse_key_value(line, "RssAnon")) {
            snapshot.rss_anon = to_uint64(*val) * 1024;
        } else if (auto val = parse_key_value(line, "RssFile")) {
            snapshot.rss_file = to_uint64(*val) * 1024;
        } else if (auto val = parse_key_value(line, "RssShmem")) {
            snapshot.rss_shmem = to_uint64(*val) * 1024;
        } else if (auto val = parse_key_value(line, "voluntary_ctxt_switches")) {
            snapshot.voluntary_ctxt_switches = to_uint64(*val);
        } else if (auto val = parse_key_value(line, "nonvoluntary_ctxt_switches")) {
            snapshot.nonvoluntary_ctxt_switches = to_uint64(*val);
        }
    }
    
    return Result<void>();
}
#endif

#if defined(__APPLE__)
Result<void> ProcReader::read_io(pid_t pid, MetricsSnapshot& snapshot) {
    (void)pid;
    snapshot.read_bytes = std::nullopt;
    snapshot.write_bytes = std::nullopt;
    snapshot.cancelled_write_bytes = std::nullopt;
    return Result<void>();
}
#else
Result<void> ProcReader::read_io(pid_t pid, MetricsSnapshot& snapshot) {
    std::string path = "/proc/" + std::to_string(pid) + "/io";
    auto result = read_file(path);
    if (!result) {
        // I/O stats may not be available for all processes (permission denied)
        // Initialize to zero and return success
        snapshot.read_bytes = std::nullopt;
        snapshot.write_bytes = std::nullopt;
        snapshot.cancelled_write_bytes = std::nullopt;
        return Result<void>();
    }
    
    const std::string& content = *result;
    std::istringstream iss(content);
    std::string line;
    
    snapshot.read_bytes = std::nullopt;
    snapshot.write_bytes = std::nullopt;
    snapshot.cancelled_write_bytes = std::nullopt;
    
    while (std::getline(iss, line)) {
        if (auto val = parse_key_value(line, "read_bytes")) {
            snapshot.read_bytes = to_uint64(*val);
        } else if (auto val = parse_key_value(line, "write_bytes")) {
            snapshot.write_bytes = to_uint64(*val);
        } else if (auto val = parse_key_value(line, "cancelled_write_bytes")) {
            snapshot.cancelled_write_bytes = to_uint64(*val);
        }
    }
    
    return Result<void>();
}
#endif

#if defined(__APPLE__)
Result<uint64_t> ProcReader::count_fds(pid_t pid) {
    const int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0) {
        return Result<uint64_t>::error("Failed to list process file descriptors");
    }

    std::vector<proc_fdinfo> fds(static_cast<size_t>(bytes / sizeof(proc_fdinfo)));
    const int filled = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), bytes);
    if (filled <= 0) {
        return Result<uint64_t>::error("Failed to read process file descriptors");
    }

    return static_cast<uint64_t>(filled / sizeof(proc_fdinfo));
}
#else
Result<uint64_t> ProcReader::count_fds(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/fd";
    
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return Result<uint64_t>::error("Failed to open FD directory");
    }
    
    uint64_t count = 0;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        // Skip "." and ".."
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    
    closedir(dir);
    return count;
}
#endif

} // namespace procmon
