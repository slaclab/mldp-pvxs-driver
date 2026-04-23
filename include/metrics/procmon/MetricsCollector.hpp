#pragma once

#include "MetricsSnapshot.hpp"
#include "MetricsHandler.hpp"
#include "Result.hpp"
#include <memory>
#include <vector>

namespace procmon {

/**
 * @brief High-level collector for process metrics
 *
 * This class orchestrates reading metrics from the platform backend and delivering
 * them to registered handlers. It provides a clean interface for
 * host programs to collect process metrics.
 */
class MetricsCollector {
public:
    /**
     * @brief Construct a collector for a specific process
     * @param pid Process ID to monitor
     */
    explicit MetricsCollector(pid_t pid);

    /**
     * @brief Register a handler to receive metrics snapshots
     * @param handler Shared pointer to handler
     */
    void register_handler(std::shared_ptr<MetricsHandler> handler);

    /**
     * @brief Collect metrics and deliver to registered handlers
     * @return Snapshot on success, error message on failure
     *
     * This method reads all metrics for the process and either
     * delivers them to registered handlers or returns them directly.
     */
    Result<MetricsSnapshot> collect();

    /**
     * @brief Get the monitored process ID
     * @return Process ID
     */
    [[nodiscard]] pid_t get_pid() const noexcept { return pid_; }

    /**
     * @brief Check if the monitored process exists
     * @return true if process exists, false otherwise
     */
    [[nodiscard]] bool is_process_alive() const;

private:
    pid_t pid_;
    std::vector<std::shared_ptr<MetricsHandler>> handlers_;

    /**
     * @brief Notify all registered handlers of a snapshot
     * @param snapshot The metrics snapshot
     */
    void notify_handlers(const MetricsSnapshot& snapshot);

    /**
     * @brief Notify all registered handlers of an error
     * @param error_message The error message
     */
    void notify_error(const std::string& error_message);
};

} // namespace procmon
