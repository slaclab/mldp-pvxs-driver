//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

/** @file ArrowTypeMap.h
 * @brief Maps query column types to their Arrow representations. */
#pragma once

#include <query/IQueryable.h>

#include <arrow/type.h>

namespace mldp_pvxs_driver::query {

/** @brief Returns the Arrow type used to represent a logical query column type.
 * @param[in] type Logical query type to map.
 * @return Matching Arrow type, or null when the logical type has no scalar Arrow mapping. */
inline std::shared_ptr<arrow::DataType> arrowType(ColumnType type)
{
    switch (type)
    {
    case ColumnType::STRING:
        return arrow::utf8();
    case ColumnType::TIMESTAMP:
        return arrow::timestamp(arrow::TimeUnit::SECOND, "UTC");
    case ColumnType::DURATION_SECONDS:
        return arrow::duration(arrow::TimeUnit::SECOND);
    case ColumnType::INT:
        return arrow::int64();
    case ColumnType::BOOL:
        return arrow::boolean();
    }
    return nullptr;
}

} // namespace mldp_pvxs_driver::query
