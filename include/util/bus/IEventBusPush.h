/** @file
 * @brief Interface describing the minimal API to push events into the driver bus.
 */

#pragma once
#include <ingestion.grpc.pb.h>
#include <memory>

namespace mldp_pvxs_driver::util::bus {

/**
 * @brief Minimal API contract for pushing events on the driver bus.
 *
 * Implementations are expected to forward serialized ingestion events to the
 * rest of the system (e.g. over gRPC or PVXS) while honoring the ownership
 * semantics of the provided payloads.
 */
class IEventBusPush {
public:
    /// Shared ownership wrapper around the generated ingestion payload.
    using EventValue = std::shared_ptr<DataValue>;

    /**
     * @brief Helper factory that returns an empty event payload.
     * @return Shared pointer users can populate before invoking @ref push.
     */
    static EventValue MakeEventValue() {
        return std::make_shared<DataValue>();
    }
    
    virtual ~IEventBusPush() = default;

    /**
     * @brief Pushes a populated ingestion event into the bus.
     * @param data_value Shared pointer describing the event contents; callers
     *                   retain ownership and may reuse the pointer after this
     *                   function returns.
     * @return true if the event was accepted for delivery.
     */
    virtual bool push(EventValue data_value) = 0;
};

} // namespace mldp_pvxs_driver::util::bus
