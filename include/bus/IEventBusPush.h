/** @file
 * @brief Interface describing the minimal API to push events into the driver bus.
 */

#pragma once

namespace mldp_pvxs_driver::bus {

class IEventBusPush {
public:
    virtual ~IEventBusPush() = default;

    /**
     * @brief Pushes an event into the bus for downstream consumers.
     * @param evt Abstract payload describing the event; concrete implementations
     *            document the expected type and ownership.
     * @return true if the push succeeded and the event will be delivered.
     */
    virtual bool push(/* Event evt */) = 0;
};

} // namespace mldp_pvxs_driver::bus
