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
 * @file EpicsPVXSBsasGen1Reader.h
 * @brief `epics-pvxs-bsas-gen1` reader: SLAC BSAS Gen1 NTTables over PVXS.
 */

#pragma once

#include <reader/ReaderFactory.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.h>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Reader for SLAC BSAS Gen1 NTTables.
 *
 * Every non-timestamp column of the table is a PV value column.  PVA field
 * names cannot contain ':', so the IOC publishes mangled field names
 * (`ACCL_IN20_300_L0A_ACUSBR`) and lists the real PV names
 * (`ACCL:IN20:300:L0A_ACUSBR`) in a companion signal PV (`<prefix>_SIG`).
 *
 * At construction, for every table PV with a signal PV (derived from a
 * trailing `_TBL` or set with `option.signal-pv`) the reader gets the signal
 * PV and the table once, checks that the data columns and the signal names
 * match one-to-one by position (column count equal, and each field equal to
 * the signal name once ':' and '_' are removed from both), and caches
 * field name -> real name.
 * Any failure throws, so the reader (and the driver) does not start.  The
 * cache is fixed for the reader lifetime; a column not in it is dropped.
 *
 * Tables without a signal PV name their columns from the NTTable `labels`,
 * falling back to the field name.
 *
 * A PV name is published by one table only.  When several tables expose the
 * same PV column (e.g. beamline-wide signals present in both CU HXR and CU
 * SXR tables), the first table claiming it owns it and the column is dropped
 * from every other table; otherwise both tables would write the same MLDP PV
 * (duplicate/overlapping buckets).  Tables with a signal PV claim at startup
 * in configuration order; label-named tables claim on first update.  See docs/readers/slac-bsas-table-gen1.md.
 *
 * @code{.yaml}
 * readers:
 *   - epics-pvxs-bsas-gen1:
 *       - name: bsas_gen1
 *         rows-per-data-batch: 120
 *         pvs-per-data-batch: 1
 *         data-batches-per-push: 50
 *         pvs:
 *           - name: "BSAS:GEN1:TABLE"
 * @endcode
 */
class EpicsPVXSBsasGen1Reader : public EpicsPVXSBsasTableReaderBase
{
public:
    static constexpr std::string_view kReaderType = "epics-pvxs-bsas-gen1";

    EpicsPVXSBsasGen1Reader(std::shared_ptr<util::bus::IDataBus> bus,
                            std::shared_ptr<metrics::Metrics>    metrics,
                            const config::Config&                cfg);

    /** @brief Column filter keeping every column under its label (field name as fallback). */
    static ColumnFilter makeColumnFilter(const TableOptions& opts);

    /** @brief Real column names of one table: field name (and real name) -> real name. */
    using ColumnNameMap = std::unordered_map<std::string, std::string>;

    /**
     * @brief Verify a table's data fields against its signal names.
     *
     * @param tablePv Table PV name (error messages).
     * @param signalPv Signal PV name (error messages).
     * @param fields Data field names of the table, in column order (timestamps excluded).
     * @param signals Real PV names from the signal PV, in column order.
     * @return Map from field name (and from real name) to real name.
     * @throws std::runtime_error on count or per-position name mismatch.
     */
    static ColumnNameMap buildColumnNameMap(const std::string&              tablePv,
                                            const std::string&              signalPv,
                                            const std::vector<std::string>& fields,
                                            const std::vector<std::string>& signals);

    /**
     * @brief Thread-safe registry of which table publishes each PV name.
     *
     * The first table claiming a name owns it; later claims by other tables fail.
     */
    class ColumnOwnership
    {
    public:
        /**
         * @brief Claim @p pvName for @p tablePv.
         * @return true if @p tablePv owns the name (new claim or already its own).
         */
        bool claim(const std::string& pvName, const std::string& tablePv);

        /** @brief Owning table of @p pvName, empty if unclaimed. */
        std::string ownerOf(const std::string& pvName) const;

    private:
        mutable std::mutex                           mutex_;
        std::unordered_map<std::string, std::string> owner_;
    };

private:
    /** @brief Resolve and verify the real column names of every table with a signal PV. */
    void resolveSignalNames();

    /** @brief Wrap the label-based filter of tables without a signal PV with ownership checks. */
    void installOwnershipForLabelTables();

    std::shared_ptr<ColumnOwnership> ownership_ = std::make_shared<ColumnOwnership>();

private:
    REGISTER_READER("epics-pvxs-bsas-gen1", EpicsPVXSBsasGen1Reader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics
