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
 * @brief Protocol-agnostic columnar data batch used as the bus frame type.
 *
 * `DataBatch` carries time-stamped, heterogeneous column data without any
 * dependency on MLDP protobuf types.  Writers that target MLDP convert
 * `DataBatch` → `dp::service::common::DataFrame` internally; other writers
 * (e.g. HDF5) consume `DataBatch` directly.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mldp_pvxs_driver::util::bus {

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

/**
 * @brief Per-sample timestamp (Unix epoch seconds + nanosecond offset).
 */
struct TimestampEntry
{
    uint64_t epoch_seconds{0}; ///< Seconds past Unix epoch.
    uint64_t nanoseconds{0};   ///< Nanosecond sub-second offset.
};

// ---------------------------------------------------------------------------
// Column types
// ---------------------------------------------------------------------------

/**
 * @brief Shape descriptor for array-valued columns.
 *
 * For a 1-D waveform of 1024 samples: `dims = {1024}`.
 * For a 64×64 image frame: `dims = {64, 64}`.
 */
struct ArrayDims
{
    std::vector<uint32_t> dims; ///< Size of each dimension.
};

/**
 * @brief Union of all supported per-column data representations.
 *
 * Scalar columns store one value per timestamp entry.
 * Array columns store one flattened array per timestamp entry; use
 * @ref DataBatch::array_dims to look up the shape for a given column name.
 *
 * | Index | C++ type                           | Semantic          |
 * |-------|------------------------------------|-------------------|
 * |   0   | `std::vector<double>`              | scalar double     |
 * |   1   | `std::vector<float>`               | scalar float      |
 * |   2   | `std::vector<int64_t>`             | scalar int64      |
 * |   3   | `std::vector<int32_t>`             | scalar int32      |
 * |   4   | `std::vector<bool>`                | scalar bool       |
 * |   5   | `std::vector<std::string>`         | scalar string     |
 * |   6   | `std::vector<std::vector<uint8_t>>`| bytes/blob/struct |
 * |   7   | `std::vector<std::vector<double>>` | double array      |
 * |   8   | `std::vector<std::vector<float>>`  | float array       |
 * |   9   | `std::vector<std::vector<int64_t>>`| int64 array       |
 * |  10   | `std::vector<std::vector<int32_t>>`| int32 array       |
 * |  11   | `std::vector<std::vector<bool>>`   | bool array        |
 */
using ColumnValues = std::variant<
    std::vector<double>,                 // 0 — scalar double
    std::vector<float>,                  // 1 — scalar float
    std::vector<int64_t>,                // 2 — scalar int64
    std::vector<int32_t>,                // 3 — scalar int32
    std::vector<bool>,                   // 4 — scalar bool
    std::vector<std::string>,            // 5 — scalar string
    std::vector<std::vector<uint8_t>>,   // 6 — bytes / struct / image blob
    std::vector<std::vector<double>>,    // 7 — double array column
    std::vector<std::vector<float>>,     // 8 — float array column
    std::vector<std::vector<int64_t>>,   // 9 — int64 array column
    std::vector<std::vector<int32_t>>,   // 10 — int32 array column
    std::vector<std::vector<bool>>       // 11 — bool array column
>;

/**
 * @brief Named column of homogeneous typed samples.
 */
struct DataColumn
{
    std::string  name;   ///< Column / signal name.
    ColumnValues values; ///< Typed sample vector (one entry per timestamp).
};

/**
 * @brief Named enum column: integer values with a user-defined semantic id.
 *
 * The @p enum_id is a contract between data producer and consumer
 * (e.g. `"epics:alarm_status:v2"`).
 */
struct EnumDataColumn
{
    std::string          name;    ///< Column / signal name.
    std::vector<int32_t> values;  ///< Enum integer values (one per timestamp).
    std::string          enum_id; ///< Semantic identifier for enum encoding.
};

// ---------------------------------------------------------------------------
// DataBatch
// ---------------------------------------------------------------------------

/**
 * @brief Protocol-agnostic, columnar data frame passed through the event bus.
 *
 * One `DataBatch` carries all samples for one ingestion event:
 * - `timestamps` — ordered list of per-sample time points.
 * - `columns`    — heterogeneous typed columns (one value per timestamp).
 * - `enum_columns` — integer enum columns with semantic ids.
 * - `array_dims`  — optional shape information keyed by column name; only
 *                   needed when a column holds array-valued samples.
 *
 * ## Usage
 *
 * ```cpp
 * util::bus::DataBatch batch;
 * batch.timestamps.push_back({epoch_sec, nanos});
 *
 * util::bus::DataColumn col;
 * col.name   = "temperature";
 * col.values = std::vector<double>{42.0};
 * batch.columns.push_back(std::move(col));
 *
 * IDataBus::EventBatch ev;
 * ev.root_source = "sensor:temperature";
 * ev.frames.push_back(std::move(batch));
 * bus->push(std::move(ev));
 * ```
 */
struct DataBatch
{
    std::vector<TimestampEntry>                timestamps;   ///< Per-sample timestamps (required).
    std::vector<DataColumn>                    columns;      ///< Typed scalar or array columns.
    std::vector<EnumDataColumn>                enum_columns; ///< Enum columns.
    std::unordered_map<std::string, ArrayDims> array_dims;   ///< Shape info keyed by column name.
};

} // namespace mldp_pvxs_driver::util::bus
