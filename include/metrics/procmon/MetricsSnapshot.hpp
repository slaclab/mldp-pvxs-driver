#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <optional>
#include <sys/types.h>

namespace procmon {

/**
 * @brief Represents a snapshot of process metrics at a point in time
 * 
 * This structure contains all relevant metrics collected from platform
 * sources for a specific process.
 */
struct MetricsSnapshot {
    // Process identification
    pid_t pid;                              ///< Process ID
    std::string comm;                       ///< Command name (from /proc/[pid]/comm)
    
    // CPU metrics (source depends on platform)
    std::optional<uint64_t> utime;          ///< User mode time (clock ticks)
    std::optional<uint64_t> stime;          ///< Kernel mode time (clock ticks)
    std::optional<uint64_t> cutime;         ///< Children user time (clock ticks)
    std::optional<uint64_t> cstime;         ///< Children kernel time (clock ticks)
    std::optional<int64_t> priority;        ///< Process priority
    std::optional<int64_t> nice;            ///< Nice value
    std::optional<int64_t> num_threads;     ///< Number of threads
    
    // Memory metrics (source depends on platform)
    std::optional<uint64_t> vm_size;        ///< Virtual memory size (bytes)
    std::optional<uint64_t> vm_rss;         ///< Resident set size (bytes)
    std::optional<uint64_t> vm_peak;        ///< Peak virtual memory size (bytes)
    std::optional<uint64_t> rss_anon;       ///< Anonymous RSS (bytes)
    std::optional<uint64_t> rss_file;       ///< File-backed RSS (bytes)
    std::optional<uint64_t> rss_shmem;      ///< Shared memory RSS (bytes)
    
    // I/O metrics (source depends on platform)
    std::optional<uint64_t> read_bytes;     ///< Bytes read from storage
    std::optional<uint64_t> write_bytes;    ///< Bytes written to storage
    std::optional<uint64_t> cancelled_write_bytes; ///< Bytes cancelled before write
    
    // Context switches (source depends on platform)
    std::optional<uint64_t> voluntary_ctxt_switches;    ///< Voluntary context switches
    std::optional<uint64_t> nonvoluntary_ctxt_switches; ///< Involuntary context switches
    
    // File descriptors (source depends on platform)
    std::optional<uint64_t> num_fds;        ///< Number of open file descriptors
    
    // Timestamp
    std::chrono::system_clock::time_point timestamp;  ///< When snapshot was taken
    
    /**
     * @brief Calculate total CPU time (user + system)
     * @return Total CPU time in clock ticks
     */
    [[nodiscard]] std::optional<uint64_t> total_cpu_time() const noexcept {
        if (!utime || !stime) {
            return std::nullopt;
        }
        return *utime + *stime;
    }
    
    /**
     * @brief Calculate total CPU time including children
     * @return Total CPU time with children in clock ticks
     */
    [[nodiscard]] std::optional<uint64_t> total_cpu_time_with_children() const noexcept {
        if (!utime || !stime || !cutime || !cstime) {
            return std::nullopt;
        }
        return *utime + *stime + *cutime + *cstime;
    }
    
    /**
     * @brief Calculate total RSS (all components)
     * @return Total RSS in bytes
     */
    [[nodiscard]] std::optional<uint64_t> total_rss() const noexcept {
        if (!rss_anon || !rss_file || !rss_shmem) {
            return std::nullopt;
        }
        return *rss_anon + *rss_file + *rss_shmem;
    }
};

} // namespace procmon
