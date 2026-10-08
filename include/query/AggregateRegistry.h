//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file AggregateRegistry.h
 * @brief Declares the catalog of SQL aggregate functions (COUNT, SUM, ...).
 *
 * Aggregates mirror scalar functions in ExpressionRegistry: each one is an
 * IAggregateFunction with a descriptor (listed by SHOW FUNCTIONS), type
 * inference used by the binder, and a factory for the column-oriented
 * IAggregateAccumulator that the GROUP BY executor drives.  New aggregates are
 * added by registering another IAggregateFunction. */
#pragma once

#include <query/ExpressionRegistry.h>
#include <query/IQueryable.h>

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/type.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query {

/** @brief Merge mapping value meaning "skip this source group". */
inline constexpr uint32_t kSkipGroup = UINT32_MAX;

/** @brief Per-aggregate state for every group, updated a batch at a time.
 *
 *  Group ids are dense, starting at 0.  The executor calls resize() before
 *  passing a new group id, consume() once per input batch, and finish() once. */
class IAggregateAccumulator
{
public:
    virtual ~IAggregateAccumulator() = default;

    /** @brief Grows the state to hold @p groups groups (never shrinks). */
    virtual void resize(std::size_t groups) = 0;

    /** @brief Folds one batch into the groups.
     * @param[in] values    Argument values, or null for an argument-less call (COUNT(*)).
     * @param[in] group_ids Group id per row.
     * @param[in] rows      Number of rows. */
    virtual void consume(const arrow::Array* values, const uint32_t* group_ids, int64_t rows) = 0;

    /** @brief Folds @p other's groups into this one; group g of @p other becomes @p mapping[g]
     *  (kSkipGroup skips it).  Merged groups must already exist (resize()).
     *
     *  @p other must come from the same function with the same input type, and
     *  must have consumed rows that follow this accumulator's rows (FIRST/LAST). */
    virtual void merge(IAggregateAccumulator& other, const std::vector<uint32_t>& mapping) = 0;

    /** @brief Builds the result array, one row per group in group-id order. */
    virtual std::shared_ptr<arrow::Array> finish(arrow::MemoryPool* pool) = 0;

    /** @brief Approximate bytes of state held, for memory accounting. */
    virtual std::size_t memoryBytes() const = 0;
};

/** @brief A registered aggregate function. */
class IAggregateFunction
{
public:
    virtual ~IAggregateFunction() = default;

    /** @brief Descriptor for SHOW FUNCTIONS (kind AGGREGATE). */
    [[nodiscard]] virtual const ExpressionCallableDescriptor& descriptor() const noexcept = 0;

    /** @brief Checks a call and returns its SQL result type.
     * @param[in] argument Argument type, or nullopt for a '*' call.
     * @param[in] distinct True for a DISTINCT call.
     * @return Result type.
     * @throws plan::PlannerException For an invalid call. */
    [[nodiscard]] virtual ColumnType bind(std::optional<ColumnType> argument, bool distinct) const = 0;

    /** @brief Creates the accumulator for one call.
     * @param[in] input    Arrow type of the argument values, or null when unknown or a '*' call.
     * @param[in] distinct True for a DISTINCT call. */
    [[nodiscard]] virtual std::unique_ptr<IAggregateAccumulator> makeAccumulator(const std::shared_ptr<arrow::DataType>& input, bool distinct) const = 0;
};

/** @brief Immutable registry of the built-in aggregate functions. */
class AggregateRegistry
{
public:
    /** @brief Returns the process-wide registry. */
    static const AggregateRegistry& instance();

    /** @brief Looks up an aggregate by name (case-insensitive); null when @p name is not an aggregate. */
    [[nodiscard]] const IAggregateFunction* find(std::string_view name) const;

    /** @brief Returns descriptors of every aggregate, sorted by name. */
    [[nodiscard]] std::vector<ExpressionCallableDescriptor> functions() const;

private:
    AggregateRegistry();
    std::vector<std::unique_ptr<IAggregateFunction>> functions_;
};

} // namespace mldp_pvxs_driver::query
