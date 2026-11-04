//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file AggregateValue.h
 * @brief Typed, allocation-free access to Arrow column values used by GROUP BY.
 *
 * ColumnReader resolves an Arrow array once and then reads each row as a small
 * ValueRef (integer, double, bool, string view, or an opaque reference for
 * nested types).  Dense and sparse unions (the native MLDP @c value column) are
 * read through their active child.  OwnedValue keeps a copied value per group,
 * and buildValueColumn() turns a vector of them back into an Arrow array. */
#pragma once

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mldp_pvxs_driver::query::executor {

/** @brief Kind of a value read from a column. */
enum class ValueKind : uint8_t
{
    NUL,    ///< Null value.
    INT,    ///< Integer-backed value (integers, timestamps, durations, dates, times).
    DOUBLE, ///< Floating-point value.
    BOOL,   ///< Boolean value.
    STRING, ///< String or binary value.
    OTHER   ///< Nested or unsupported type; read through the source array.
};

/** @brief Non-owning view of one column value. */
struct ValueRef
{
    ValueKind          kind{ValueKind::NUL}; ///< Value kind.
    int64_t            integer{0};           ///< Value for ValueKind::INT.
    double             real{0};              ///< Value for ValueKind::DOUBLE.
    bool               boolean{false};       ///< Value for ValueKind::BOOL.
    std::string_view   text;                 ///< Value for ValueKind::STRING; valid while the source array lives.
    const arrow::Array* array{nullptr};      ///< Source array for ValueKind::OTHER.
    int64_t            index{0};             ///< Row in @c array for ValueKind::OTHER.

    /** @brief True for INT and DOUBLE values. */
    bool numeric() const noexcept { return kind == ValueKind::INT || kind == ValueKind::DOUBLE; }
    /** @brief Numeric value as double; requires numeric(). */
    double asDouble() const noexcept { return kind == ValueKind::INT ? static_cast<double>(integer) : real; }
};

/** @brief Reads rows of one Arrow array as ValueRef without per-row allocation. */
class ColumnReader
{
public:
    /** @brief Prepares a reader for @p array, which must outlive the reader. */
    explicit ColumnReader(const arrow::Array& array);
    ~ColumnReader();
    ColumnReader(ColumnReader&&) noexcept;
    ColumnReader& operator=(ColumnReader&&) noexcept;

    /** @brief Returns the value at @p row. */
    ValueRef at(int64_t row) const;

private:
    const arrow::Array*                          array_;
    arrow::Type::type                            id_;
    const void*                                  values_{nullptr};
    bool                                         dense_union_{false};
    const int8_t*                                type_codes_{nullptr};
    const int32_t*                               value_offsets_{nullptr};
    std::vector<std::shared_ptr<arrow::Array>>   union_children_;      ///< Indexed by type code.
    std::vector<std::unique_ptr<ColumnReader>>   union_readers_;       ///< Indexed by type code.
};

/** @brief Owned copy of one value, kept per group. */
struct OwnedValue
{
    ValueKind                      kind{ValueKind::NUL}; ///< Value kind.
    int64_t                        integer{0};           ///< Value for ValueKind::INT.
    double                         real{0};              ///< Value for ValueKind::DOUBLE.
    bool                           boolean{false};       ///< Value for ValueKind::BOOL.
    std::string                    text;                 ///< Value for ValueKind::STRING.
    std::shared_ptr<arrow::Scalar> other;                ///< Value for ValueKind::OTHER.

    /** @brief Copies @p value into an owned value. */
    static OwnedValue from(const ValueRef& value);
    /** @brief Approximate heap bytes held beyond sizeof(OwnedValue). */
    std::size_t heapBytes() const noexcept { return text.capacity() > 15 ? text.capacity() : 0; }
};

/** @brief Orders two non-null values: numbers numerically, strings lexically, booleans false < true.
 * @return Negative, zero, or positive.
 * @throws std::runtime_error For values of incomparable kinds. */
int compareValues(const ValueRef& lhs, const OwnedValue& rhs);

/** @brief Appends a type-tagged, length-prefixed encoding of @p value to @p out (grouping / DISTINCT key). */
void appendValueKey(std::string& out, const ValueRef& value);

/** @brief Returns the input type an accumulator should preserve: @p type unless it is a union or null type. */
std::shared_ptr<arrow::DataType> preservedType(const std::shared_ptr<arrow::DataType>& type);

/** @brief Builds an Arrow array from owned values.
 *
 *  With a non-null @p type the values are rebuilt in that type (integer-backed
 *  types such as timestamps keep their unit).  Without one the type is chosen
 *  from the kinds present: int64, double (mixed numbers), bool, utf8, or utf8
 *  text when kinds are mixed; all-null input yields a null-type array.
 * @param[in] values Values in output row order.
 * @param[in] type   Preferred output type, or null.
 * @param[in] pool   Memory pool.
 * @return Built array. */
std::shared_ptr<arrow::Array> buildValueColumn(const std::vector<OwnedValue>& values,
                                               const std::shared_ptr<arrow::DataType>& type,
                                               arrow::MemoryPool* pool);

} // namespace mldp_pvxs_driver::query::executor
