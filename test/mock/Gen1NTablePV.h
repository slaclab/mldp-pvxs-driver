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

#include <string>
#include <vector>

#include <pvxs/server.h>
#include <pvxs/sharedpv.h>

/**
 * @brief Generic BSAS Gen1 NTTable mock PV.
 *
 * Loads column names from a plain-text signals file (one PV name per line).
 * All data columns are Float64.  Two per-row timestamp columns are appended:
 *   value.secondsPastEpoch  UInt32[]
 *   value.nanoseconds       UInt32[]
 *
 * Multiple instances can be created with different PV names and signal files,
 * e.g. one for cu-hxr and one for cu-sxr.
 */
class Gen1NTablePV
{
public:
    static constexpr int kRows = 3; ///< rows (beam pulses) per update

    /**
     * Real IOC layout: the table is published as `<baseName>_TBL` (sanitized
     * field names, `labels` repeating them) and the real PV names as a static
     * string-array PV `<baseName>_SIG`, in column order.
     *
     * @param baseName    PV name prefix (e.g. "CU-HXR" -> CU-HXR_TBL / CU-HXR_SIG)
     * @param signalsFile Path to a plain-text file with one signal name per line
     */
    Gen1NTablePV(std::string baseName, std::string signalsFile);

    /// Table PV name (`<baseName>_TBL`).
    const std::string& tablePvName() const { return m_pvName; }

    /// Signal PV name (`<baseName>_SIG`).
    const std::string& signalPvName() const { return m_signalPvName; }

    /// Register the PV with @p server.  Must be called before server.start().
    void registerPV(pvxs::server::Server& server);

    /// Post one update with kRows rows of synthetic sinusoidal data.
    void post(long long seconds, long long nanos, int counter, double time);

    /// Sanitize an EPICS PV name to a valid PVXS field name.
    /// Replaces any character not in [A-Za-z0-9_] with '_'.
    static std::string sanitizeFieldName(const std::string& pvName);

    /// Original PV names as loaded from the signals file (used for labels).
    const std::vector<std::string>& columnNames() const { return m_columnNames; }

    /// Sanitized field names used in the NTTable schema.
    const std::vector<std::string>& fieldNames() const { return m_fieldNames; }

private:
    std::string              m_pvName;
    std::string              m_signalPvName;
    std::vector<std::string> m_columnNames;      ///< original PV names (used for labels)
    std::vector<std::string> m_fieldNames;       ///< sanitized field names (used in NTTable schema)
    pvxs::server::SharedPV   m_pv;
    pvxs::server::SharedPV   m_signalPv;
};
