//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <pvxs/data.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>

/**
 * @brief Deterministic SLAC BSAS NTTable mock PV (Gen1 or Gen2 layout).
 *
 * Every update derives its content from the update counter only, so tests can
 * assert exact values:
 * - row timestamps: seconds = kBaseSeconds + counter, nanoseconds = row * kRowNanosStep;
 * - each data column value = Column::value(counter, row).
 *
 * Timestamp columns are appended after the data columns unless disabled
 * (empty field names).  `labels` lists Column::label for each data column plus
 * the timestamp field names, unless labels are disabled.
 */
class BsasTablePV
{
public:
    static constexpr uint32_t kBaseSeconds = 1'700'000'000u;
    static constexpr uint32_t kRowNanosStep = 1000u;

    struct Column
    {
        std::string                         field; ///< NTTable field name (sanitized).
        std::string                         label; ///< `labels` entry (real name); field name when empty.
        pvxs::TypeCode                      type;  ///< Element type (Float64, Float32, Int32, UInt32, Int64, UInt64).
        std::function<double(int, size_t)>  value; ///< (counter, row) -> value; NaN allowed for float types.
    };

    struct Options
    {
        size_t      rows = 4;                       ///< Rows per update.
        std::string tsSecondsField = "secondsPastEpoch"; ///< Empty = no timestamp columns.
        std::string tsNanosField = "nanoseconds";
        bool        withLabels = true;              ///< Publish the `labels` array.
    };

    BsasTablePV(std::string pvName, std::vector<Column> columns, Options options);

    /// Register the PV with @p server.  Must be called before server.start().
    void registerPV(pvxs::server::Server& server);

    /// Post one deterministic update for @p counter.
    void post(int counter);

    const std::string& pvName() const { return m_pvName; }

private:
    std::string            m_pvName;
    std::vector<Column>    m_columns;
    Options                m_options;
    pvxs::server::SharedPV m_pv;
};
