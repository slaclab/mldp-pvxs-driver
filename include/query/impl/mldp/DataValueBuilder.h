//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file DataValueBuilder.h
 * @brief Arrow dense-union builder for all MLDP DataValue variants. */
#pragma once

#include <common.pb.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_nested.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/builder_union.h>
#include <arrow/type.h>

#include <memory>
#include <string_view>

namespace mldp_pvxs_driver::query::impl::mldp {

/** @brief Returns a string_view type name for a DataValue union variant (e.g. "float", "int64"). */
std::string_view dataValueKind(const dp::service::common::DataValue& value);

/** @brief Appends MLDP DataValue protobufs into a dense-union Arrow array. */
class DataValueBuilder
{
public:
    explicit DataValueBuilder(arrow::MemoryPool* pool);

    void append(const dp::service::common::DataValue& value);

    std::shared_ptr<arrow::Array> finish();

private:
    template <typename Builder, typename Value>
    void append(int8_t type_id, Builder& builder, const Value& value);

    std::shared_ptr<arrow::StringBuilder>    string_;
    std::shared_ptr<arrow::BooleanBuilder>   boolean_;
    std::shared_ptr<arrow::UInt32Builder>    uint32_;
    std::shared_ptr<arrow::UInt64Builder>    uint64_;
    std::shared_ptr<arrow::Int32Builder>     int32_;
    std::shared_ptr<arrow::Int64Builder>     int64_;
    std::shared_ptr<arrow::FloatBuilder>     float_;
    std::shared_ptr<arrow::DoubleBuilder>    double_;
    std::shared_ptr<arrow::BinaryBuilder>    binary_;
    std::shared_ptr<arrow::TimestampBuilder> timestamp_;
    std::shared_ptr<arrow::BinaryBuilder>    array_;
    std::shared_ptr<arrow::BinaryBuilder>    structure_;
    std::shared_ptr<arrow::BinaryBuilder>    image_;
    arrow::DenseUnionBuilder                 union_builder_;
};

} // namespace mldp_pvxs_driver::query::impl::mldp
