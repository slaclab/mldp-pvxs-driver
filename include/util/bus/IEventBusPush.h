/** @file
 * @brief Interface describing the minimal API to push events into the driver bus.
 */

#pragma once
#include <ingestion.grpc.pb.h>
#include <memory>

namespace mldp_pvxs_driver::util::bus {

class IEventBusPush {
public:
    using EventValue = std::shared_ptr<DataValue>;
    static EventValue MakeEventValue() {
        return std::make_shared<DataValue>();
    }
    
    virtual ~IEventBusPush() = default;

    /**
     * @brief Pushes an event into the bus for downstream consumers.
     * @param evt Abstract payload describing the event; concrete implementations
     *            document the expected type and ownership.
     * @return true if the push succeeded and the event will be delivered.
     */
    virtual bool push(EventValue data_value) = 0;
};

} // namespace mldp_pvxs_driver::bus
