// Reader.hpp
#pragma once

#include <memory>
#include <string>

namespace mldp_pvxs_driver::util::bus {
class IEventBusPush;
}

namespace mldp_pvxs_driver::reader {

/**
 * @brief Contract for reader implementations that sample data and forward events to the bus.
 *
 * Derived classes register with @c ReaderFactory and implement sensor-specific polling logic.
 */
class Reader
{
public:
    /** @brief Construct the reader with the event bus connection that will receive updates. */
    explicit Reader(std::shared_ptr<util::bus::IEventBusPush> bus)
        : bus_(std::move(bus)) {}

    virtual ~Reader() = default;

    /** @brief Return the human-readable identifier for this reader instance. */
    virtual std::string name() const = 0;

protected:
    /** @brief Event bus that derived readers use to deliver events.
     *
     * Ownership is shared so readers can outlive their producers.
     */
    std::shared_ptr<util::bus::IEventBusPush> bus_;
};

} // namespace mldp_pvxs_driver::reader
