//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file
 * @brief Interface describing the minimal API to push events into the driver bus.
 */

#pragma once
#include <cstdint>
#include <ingestion.grpc.pb.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::util::bus {

/**
 * @brief Timestamped ingestion payload shared across the bus.
 *
 * Instances carry both coarse and fine grained timestamps along with the
 * generated @ref DataValue, allowing transport layers to forward the payload
 * without copying.
 */
struct EventValueStruct
{
    const uint64_t             epoch_seconds; ///< Unix epoch seconds for the sample.
    const uint64_t             nanoseconds;   ///< Additional nanoseconds precision.
    std::shared_ptr<DataValue> data_value;
};

/**
 * @brief Container describing a batch of events to ingest.
 *
 * Tags provide additional metadata that should accompany the entire batch,
 * while @ref values holds the individual payloads grouped by their signal name.
 */
struct EventBatchStruct
{
    std::vector<std::string> tags; ///< Optional metadata attached to the batch.
    std::map<std::string, std::vector<std::shared_ptr<EventValueStruct>>> values; ///< Payloads grouped per source ID.
};

/**
 * @brief Minimal API contract for pushing events on the driver bus.
 *
 * Implementations are expected to forward serialized ingestion events to the
 * rest of the system (e.g. over gRPC or PVXS) while honoring the ownership
 * semantics of the provided payloads.
 */
class IEventBusPush
{
public:
    /// Shared ownership wrapper around the generated ingestion payload.
    using EventValue = std::shared_ptr<EventValueStruct>;
    /// Batch of values grouped per source identifier with optional tags.
    using EventBatch = EventBatchStruct;

    /**
     * @brief Helper factory that returns an empty event payload.
     * @return Shared pointer users can populate before invoking @ref push.
     */
    static EventValue MakeEventValue(uint64_t epoch_seconds = 0, uint64_t nanoseconds = 0)
    {
        // Construct a temporary aggregate explicitly to avoid overload
        // resolution issues with braced-init-lists and make_shared.
        return std::make_shared<EventValueStruct>(
            EventValueStruct{
                epoch_seconds,
                nanoseconds,
                std::make_shared<DataValue>()});
    }

    virtual ~IEventBusPush() = default;

    /**
     * @brief Pushes a batch of populated ingestion events into the bus.
     *
     * Each entry in @p batch_values.values represents a single source whose
     * vector aggregates one or more payloads. Implementations may forward all
     * entries in a single call to the back-end to minimize network round-trips.
     * When @p batch_values.tags is empty, implementations may add their own
     * default tags (such as each source name) before forwarding the batch.
     *
     * @param batch_values Aggregated batch describing tags and payloads. Each
     *                     payload is shared with the bus implementation.
     * @return true if the batch was accepted for delivery.
     */
    virtual bool push(EventBatch batch_values) = 0;
};

} // namespace mldp_pvxs_driver::util::bus
