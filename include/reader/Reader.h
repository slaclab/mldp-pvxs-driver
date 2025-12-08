// Reader.hpp
#pragma once

#include <memory>
#include <string>

namespace mldp_pvxs_driver::metrics {
class Metrics;
}

namespace mldp_pvxs_driver::util::bus {
class IEventBusPush;
}

namespace mldp_pvxs_driver::reader {

using ReaderUPtr = std::unique_ptr<class Reader>;
    
/**
 * @brief Contract for reader implementations that sample data and forward events to the bus.
 *
 * Derived classes register with @c ReaderFactory and implement sensor-specific polling logic.
 */
class Reader
{
public:
    /** @brief Construct the reader with the event bus connection that will receive updates. */
    Reader(std::shared_ptr<util::bus::IEventBusPush> bus,
           std::shared_ptr<metrics::Metrics>         metrics = nullptr)
        : bus_(std::move(bus))
        , metrics_(std::move(metrics)) {}

    virtual ~Reader() = default;

    /** @brief Return the human-readable identifier for this reader instance. */
    virtual std::string name() const = 0;

    /** @brief Start the reader's polling loop with an optional timeout.
     *
     * The reader implementation should periodically check for timeout and exit the loop
     * when the specified duration has elapsed.
     *
     * @param timeout Timeout in milliseconds; if zero or negative, run indefinitely.
     */
    virtual void run(int timeout) = 0;

protected:
    /** @brief Event bus that derived readers use to deliver events.
     *
     * Ownership is shared so readers can outlive their producers.
     */
    std::shared_ptr<util::bus::IEventBusPush> bus_;
    /** @brief Shared metrics collector (may be null when not configured). */
    std::shared_ptr<metrics::Metrics>         metrics_;
};

} // namespace mldp_pvxs_driver::reader
