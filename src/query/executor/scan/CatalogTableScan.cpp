//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryTableCatalog.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::query;
using namespace mldp_pvxs_driver::query::executor;

RecordBatches mldp_pvxs_driver::query::executor::readCatalogTable(const plan::PhysicalTableScan& scan, const ExecutionContext& context)
{
    if (!context.table_catalog) throw std::runtime_error("Arrow IPC table scan has no catalog");
    const auto table = context.table_catalog->find(scan.table_name);
    if (!table) throw std::runtime_error("Stored table disappeared: " + scan.table_name);
    if (table->path != scan.ipc_path) throw std::runtime_error("Stored table path changed during planning: " + scan.table_name);
    auto batches = context.table_catalog->read(*table);
    if (!batches.ok()) throw std::runtime_error(batches.status().ToString());
    return *batches;
}
