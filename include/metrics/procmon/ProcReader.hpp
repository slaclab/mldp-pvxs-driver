#pragma once

#include "MetricsSnapshot.hpp"
#include "Result.hpp"
#include <string>
#include <optional>
#include <sys/types.h>

namespace procmon {

/**
 * @brief Low-level reader for platform process metrics
 * 
 * This class provides methods to read and parse platform-specific
 * process metrics into structured data.
 */
class ProcReader {
public:
    /**
     * @brief Read the /proc/[pid]/stat file
     * @param pid Process ID
     * @return Parsed data or error message
     */
    static Result<void> read_stat(pid_t pid, MetricsSnapshot& snapshot);
    
    /**
     * @brief Read the /proc/[pid]/status file
     * @param pid Process ID
     * @return Parsed data or error message
     */
    static Result<void> read_status(pid_t pid, MetricsSnapshot& snapshot);
    
    /**
     * @brief Read the /proc/[pid]/io file
     * @param pid Process ID
     * @return Parsed data or error message
     */
    static Result<void> read_io(pid_t pid, MetricsSnapshot& snapshot);
    
    /**
     * @brief Read the /proc/[pid]/comm file
     * @param pid Process ID
     * @return Command name or error message
     */
    static Result<std::string> read_comm(pid_t pid);
    
    /**
     * @brief Count open file descriptors in /proc/[pid]/fd
     * @param pid Process ID
     * @return Number of FDs or error message
     */
    static Result<uint64_t> count_fds(pid_t pid);
    
    /**
     * @brief Check if a process exists
     * @param pid Process ID
     * @return true if process exists, false otherwise
     */
    static bool process_exists(pid_t pid);

private:
    /**
     * @brief Read entire file into string
     * @param path File path
     * @return File contents or error message
     */
    static Result<std::string> read_file(const std::string& path);
    
    /**
     * @brief Parse a key-value line from /proc files
     * @param line Line to parse
     * @param key Expected key
     * @return Value or empty optional
     */
    static std::optional<std::string> parse_key_value(const std::string& line, const std::string& key);
    
    /**
     * @brief Convert string to uint64_t
     * @param str String to convert
     * @return Converted value or 0 on error
     */
    static uint64_t to_uint64(const std::string& str) noexcept;
    
    /**
     * @brief Convert string to int64_t
     * @param str String to convert
     * @return Converted value or 0 on error
     */
    static int64_t to_int64(const std::string& str) noexcept;
};

} // namespace procmon
