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
     * @param pvName      EPICS PV name to register (e.g. "SLAC:NSTABLE:GEN1")
     * @param signalsFile Path to a plain-text file with one signal name per line
     */
    Gen1NTablePV(std::string pvName, std::string signalsFile);

    /// Register the PV with @p server.  Must be called before server.start().
    void registerPV(pvxs::server::Server& server);

    /// Post one update with kRows rows of synthetic sinusoidal data.
    void post(long long seconds, long long nanos, int counter, double time);

    /// Sanitize an EPICS PV name to a valid PVXS field name.
    /// Replaces any character not in [A-Za-z0-9_] with '_'.
    static std::string sanitizeFieldName(const std::string& pvName);

private:
    std::string              m_pvName;
    std::vector<std::string> m_columnNames;      ///< original PV names (used for labels)
    std::vector<std::string> m_fieldNames;       ///< sanitized field names (used in NTTable schema)
    pvxs::server::SharedPV   m_pv;
};
