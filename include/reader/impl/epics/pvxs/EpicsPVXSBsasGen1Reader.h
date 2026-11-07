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
#include <reader/impl/epics/pvxs/EpicsPVXSBsasTableReaderBase.h>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Reader for SLAC BSAS Gen1 NTTables.
 *
 * Every non-timestamp column of the table is a PV value column and is
 * forwarded under its field name.  See docs/readers/slac-bsas-table-gen1.md.
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

private:
    REGISTER_READER("epics-pvxs-bsas-gen1", EpicsPVXSBsasGen1Reader)
};

} // namespace mldp_pvxs_driver::reader::impl::epics
