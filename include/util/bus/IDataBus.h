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
#include <util/bus/DataBatch.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::util::bus {

/**
 * @brief Payload carrying time-series data frames for ingestion.
 *
 * Replaces the flat frame fields that were previously on EventBatchStruct.
 * @ref frames holds the individual payloads grouped by signal name.
 */
struct TimeSeriesPayload
{
    std::vector<util::bus::DataBatch> frames;                    ///< One DataBatch per ingestion payload; each batch must include timestamps.
    bool                              end_of_batch_group{false}; ///< Flush sentinel. Signals that all column batches for one logical row-synchronized group have been emitted. Writers should flush any accumulated tabular state when this is true.
    bool                              is_tabular{false};         ///< True when this batch carries column frames for a multi-column, row-synchronized table. Writers that support tabular layout accumulate column batches before flushing.
};

/**
 * @brief Metadata record for a single source/signal.
 *
 * All fields are optional to accommodate providers that only supply partial metadata.
 */
struct SourceMetadataEntry
{
    std::optional<std::vector<std::string>>      aliases;     ///< Alternative names for the source.
    std::optional<std::vector<std::string>>      tags;        ///< Descriptive tags attached to the source.
    std::unordered_map<std::string, std::string> attributes;  ///< Arbitrary key/value attributes.
    std::optional<std::string>                   description; ///< Human-readable description.
    std::optional<std::string>                   modified_by; ///< Identity of last modifier.
};

/// Map of source name to its metadata record; used as the SourceMetadata payload type.
using SourceMetadataPayload = std::unordered_map<std::string, SourceMetadataEntry>;

/**
 * @brief Nanosecond-resolution timestamp used inside bus payloads.
 */
struct BusTimestamp
{
    uint64_t epoch_seconds{0}; ///< Unix epoch seconds.
    uint64_t nanoseconds{0};   ///< Nanoseconds fraction within the second.
};

/**
 * @brief Payload describing a named configuration object and its attributes.
 */
struct ConfigurationPayload
{
    std::string                                  configuration_name;        ///< Unique name of the configuration.
    std::string                                  category;                  ///< Logical category/grouping.
    std::optional<std::string>                   description;               ///< Human-readable description.
    std::optional<std::string>                   parent_configuration_name; ///< Parent configuration name, if hierarchical.
    std::optional<std::vector<std::string>>      tags;                      ///< Descriptive tags.
    std::unordered_map<std::string, std::string> attributes;                ///< Arbitrary key/value attributes.
    std::optional<std::string>                   modified_by;               ///< Identity of last modifier.
};

/**
 * @brief Payload describing a configuration activation window.
 *
 * Records which configuration was activated, for how long, and optional
 * annotation fields (description, tags, attributes).
 */
struct ConfigurationActivationPayload
{
    std::optional<std::string>                   client_activation_id; ///< Caller-supplied idempotency key.
    std::string                                  configuration_name;   ///< Name of the activated configuration.
    BusTimestamp                                 start_time;           ///< Activation start (inclusive).
    std::optional<BusTimestamp>                  end_time;             ///< Activation end; absent means open-ended.
    std::optional<std::string>                   description;          ///< Human-readable description.
    std::optional<std::vector<std::string>>      tags;                 ///< Descriptive tags.
    std::unordered_map<std::string, std::string> attributes;           ///< Arbitrary key/value attributes.
    std::optional<std::string>                   modified_by;          ///< Identity of last modifier.
};

/**
 * @brief Discriminated union of all payload types that can travel on the bus.
 */
using BatchPayload = std::variant<TimeSeriesPayload,
                                  SourceMetadataPayload,
                                  ConfigurationPayload,
                                  ConfigurationActivationPayload>;

/**
 * @brief Container describing a batch of events to ingest.
 *
 * Metadata provides key/value annotations that accompany the entire batch.
 * The concrete payload is carried by @ref payload as a @ref BatchPayload variant.
 */
struct EventBatchStruct
{
    /// Identity of the producing reader (set by reader before push).
    std::string                                  reader_name;
    std::string                                  root_source; ///< Root PV identifier used for batch-level metrics/correlation.
    std::unordered_map<std::string, std::string> metadata;    ///< Key/value metadata annotations attached to the batch (e.g. "source" -> PV name).
    BatchPayload                                 payload;     ///< Variant payload; inspect with isTimeSeries() / asTimeSeries() helpers etc.
};

/// @name BatchPayload type-query and accessor helpers
///@{

/// Returns true when @p b carries a TimeSeriesPayload.
inline bool isTimeSeries(const EventBatchStruct& b) {
    return std::holds_alternative<TimeSeriesPayload>(b.payload);
}
/// Returns true when @p b carries a SourceMetadataPayload.
inline bool isSourceMetadata(const EventBatchStruct& b) {
    return std::holds_alternative<SourceMetadataPayload>(b.payload);
}
/// Returns true when @p b carries a ConfigurationPayload.
inline bool isConfiguration(const EventBatchStruct& b) {
    return std::holds_alternative<ConfigurationPayload>(b.payload);
}
/// Returns true when @p b carries a ConfigurationActivationPayload.
inline bool isConfigurationActivation(const EventBatchStruct& b) {
    return std::holds_alternative<ConfigurationActivationPayload>(b.payload);
}
/// Returns the TimeSeriesPayload; throws std::bad_variant_access if not the active alternative.
inline const TimeSeriesPayload& asTimeSeries(const EventBatchStruct& b) {
    return std::get<TimeSeriesPayload>(b.payload);
}
/// Returns the SourceMetadataPayload; throws std::bad_variant_access if not the active alternative.
inline const SourceMetadataPayload& asSourceMetadata(const EventBatchStruct& b) {
    return std::get<SourceMetadataPayload>(b.payload);
}
/// Returns the ConfigurationPayload; throws std::bad_variant_access if not the active alternative.
inline const ConfigurationPayload& asConfiguration(const EventBatchStruct& b) {
    return std::get<ConfigurationPayload>(b.payload);
}
/// Returns the ConfigurationActivationPayload; throws std::bad_variant_access if not the active alternative.
inline const ConfigurationActivationPayload& asConfigurationActivation(const EventBatchStruct& b) {
    return std::get<ConfigurationActivationPayload>(b.payload);
}

///@}

/**
 * @brief Normalized timestamp payload returned by source-metadata queries.
 */
struct SourceTimestamp
{
    uint64_t epoch_seconds{0}; ///< Unix epoch seconds.
    uint64_t nanoseconds{0};   ///< Nanoseconds fraction.
};

/**
 * @brief Metadata summary for one source/PV as returned by MLDP query services.
 *
 * Most fields are optional because upstream providers may not populate all
 * metadata dimensions for every source.
 */
struct SourceInfoStruct
{
    std::string                    source_name;                      ///< Source/PV identifier.
    std::optional<SourceTimestamp> first_timestamp;                  ///< Earliest known data timestamp.
    std::optional<SourceTimestamp> last_timestamp;                   ///< Latest known data timestamp.
    std::optional<std::string>     last_provider_id;                 ///< Last provider ID that wrote the source.
    std::optional<std::string>     last_provider_name;               ///< Last provider name that wrote the source.
    std::optional<std::string>     last_bucket_id;                   ///< Backing storage bucket identifier.
    std::optional<std::string>     last_bucket_data_type;            ///< Data type recorded in last bucket.
    std::optional<std::string>     last_bucket_data_timestamps_type; ///< Timestamp encoding used in last bucket.
    std::optional<uint64_t>        last_bucket_sample_period;        ///< Sampling period in nanoseconds (if known).
    std::optional<uint32_t>        last_bucket_sample_count;         ///< Number of samples in last bucket.
    std::optional<int32_t>         num_buckets;                      ///< Total bucket count for the source.
};

/**
 * @brief Tuning options for source data queries.
 *
 * Grouping these parameters avoids API churn when query knobs evolve.
 */
struct QuerySourcesDataOptions
{
    std::chrono::milliseconds timeout{std::chrono::seconds(5)};          ///< Total polling budget.
    std::chrono::seconds      lookback_window{std::chrono::seconds(30)}; ///< beginTime offset from now.
    std::chrono::seconds      forward_window{std::chrono::seconds(1)};   ///< endTime offset from now.
    std::chrono::seconds      rpc_deadline{std::chrono::seconds(5)};     ///< Per-RPC deadline.
};

/**
 * @brief Minimal API contract for pushing events on the driver bus.
 *
 * Implementations are expected to forward serialized ingestion events to the
 * rest of the system (e.g. over gRPC or PVXS) while honoring the ownership
 * semantics of the provided payloads.
 */
class IDataBus
{
public:
    /// Batch of values grouped per source identifier with optional tags.
    using EventBatch = EventBatchStruct;
    /// Metadata describing one source/PV from MLDP query APIs.
    using SourceInfo = SourceInfoStruct;

    virtual ~IDataBus() = default;

    /**
     * @brief Pushes a batch of populated ingestion events into the bus.
     *
     * Each entry in @p batch_values.frames represents one ingestion payload.
     * Implementations may forward all entries in a single call to the back-end
     * to minimize network round-trips.
     * When @p batch_values.metadata is empty, implementations may add their own
     * default metadata before forwarding the batch.
     *
     * @param batch_values Aggregated batch describing tags and payloads. Each
     *                     payload is shared with the bus implementation.
     * @return true if the batch was accepted for delivery.
     */
    virtual bool push(EventBatch batch_values) = 0;
};

} // namespace mldp_pvxs_driver::util::bus
