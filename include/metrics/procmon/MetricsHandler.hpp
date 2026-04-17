#pragma once

#include "MetricsSnapshot.hpp"
#include <memory>

namespace procmon {

/**
 * @brief Interface for handling metrics snapshots
 * 
 * This interface defines how a host program can receive and process
 * metrics snapshots. Implement this interface to handle metrics in
 * your application.
 */
class MetricsHandler {
public:
    virtual ~MetricsHandler() = default;
    
    /**
     * @brief Called when a new metrics snapshot is available
     * @param snapshot The collected metrics snapshot
     * 
     * This method will be called by the MetricsCollector when new
     * metrics have been collected for a process.
     */
    virtual void onMetricsSnapshot(const MetricsSnapshot& snapshot) = 0;
    
    /**
     * @brief Called when an error occurs during metrics collection
     * @param error_message Description of the error
     * 
     * This method is called when the collector encounters an error
     * reading metrics from /proc.
     */
    virtual void onError(const std::string& error_message) = 0;
};

} // namespace procmon
