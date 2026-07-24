//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/executor/ScanExecutionHelpers.h>

#include <query/QueryTableCatalog.h>

#include <stdexcept>

namespace mldp_pvxs_driver::query::executor {

RecordBatches readCatalogTable(const plan::PhysicalTableScan& scan, const ExecutionContext& context)
{
    if (!context.table_catalog) throw std::runtime_error("Arrow IPC table scan has no catalog");
    const auto table = context.table_catalog->find(scan.table_name);
    if (!table) throw std::runtime_error("Stored table disappeared: " + scan.table_name);
    if (table->path != scan.ipc_path) throw std::runtime_error("Stored table path changed during planning: " + scan.table_name);
    auto batches = context.table_catalog->read(*table);
    if (!batches.ok()) throw std::runtime_error(batches.status().ToString());
    return *batches;
}

} // namespace mldp_pvxs_driver::query::executor
