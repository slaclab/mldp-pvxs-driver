//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file EpicsPVXSBsasGen2Reader.h
 * @brief `epics-pvxs-bsas-gen2` reader: SLAC BSAS Gen2 NTTables over PVXS.
 */

#pragma once

#include <reader/ReaderFactory.h>
#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.h>

#include <optional>
#include <string>
#include <string_view>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Reader for SLAC BSAS Gen2 NTTables.
 *
 * Ingests the same data as Gen1: the row timestamps and, for each acquired
 * PV, only its `<PV>.Val` column, renamed to `<PV>`.  The pulse ID column and
 * the `CNT`, `RMS`, `AVG`, `MIN`, `MAX` statistics are skipped before any
 * conversion.  See docs/readers/slac-bsas-table-gen2.md.
 *
 * Column names come from the NTTable `labels` (real PV names such as
 * `XCOR:LI28:202:BACT.Val`) when they map 1:1 onto the `value.*` fields,
 * otherwise from the (sanitized) field names such as `XCOR_LI28_202_BACT_Val`.
 *
 * @code{.yaml}
 * readers:
 *   - epics-pvxs-bsas-gen2:
 *       - name: bsas_gen2
 *         pidField: pid
 *         valueSuffix: Val
 *         pvs:
 *           - name: "BSAS:GEN2:TABLE"
 * @endcode
 */
class EpicsPVXSBsasGen2Reader : public EpicsPVXSBsasTableReaderBase
{
public:
    static constexpr std::string_view kReaderType = "epics-pvxs-bsas-gen2";

    EpicsPVXSBsasGen2Reader(std::shared_ptr<util::bus::IDataBus> bus,
                            std::shared_ptr<metrics::Metrics>    metrics,
                            const config::Config&                cfg);

    /**
     * @brief Map a Gen2 column label to its output PV name.
     *
     * Returns nullopt for the pulse ID column, the timestamp columns, labels
     * without a separator or base name, and labels whose suffix (after the
     * last `.`, or the last `_` when there is no `.`) is not
     * `opts.valueSuffix` (case-insensitive).  Otherwise returns the base
     * name (the label without separator and suffix).
     */
    static std::optional<std::string> valueColumnName(std::string_view label, const TableOptions& opts);

    /** @brief Column filter applying valueColumnName() with @p opts. */
    static ColumnFilter makeColumnFilter(const TableOptions& opts);

private:
    REGISTER_READER("epics-pvxs-bsas-gen2", EpicsPVXSBsasGen2Reader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics
