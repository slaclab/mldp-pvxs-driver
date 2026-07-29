//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <arrow/filesystem/filesystem.h>
#include <arrow/record_batch.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query {

class IRecordBatchStream;

enum class TableLifetime { Session, Persistent };

struct CatalogTable {
    std::string                    name;
    TableLifetime                  lifetime{TableLifetime::Session};
    std::string                    path;
    std::shared_ptr<arrow::Schema> schema;
    int64_t                        row_count{0};
    int64_t                        byte_count{0};
};

/** Owns Arrow IPC snapshots made by CREATE [TEMP] TABLE statements. */
class QueryTableCatalog
{
public:
    QueryTableCatalog(std::shared_ptr<arrow::fs::FileSystem> file_system, std::string root_directory);
    ~QueryTableCatalog();

    QueryTableCatalog(const QueryTableCatalog&) = delete;
    QueryTableCatalog& operator=(const QueryTableCatalog&) = delete;

    arrow::Status create(std::string name, TableLifetime lifetime,
                         const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);
    /** Drain a pull stream into one Arrow IPC snapshot. */
    arrow::Status create(std::string name, TableLifetime lifetime, IRecordBatchStream& stream);
    arrow::Status drop(const std::string& name);
    [[nodiscard]] std::optional<CatalogTable> find(const std::string& name) const;
    [[nodiscard]] std::vector<CatalogTable> tables() const;
    [[nodiscard]] std::vector<std::string> tableNames() const;
    arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> read(const CatalogTable& table) const;
    arrow::Status cleanupSession();

private:
    std::shared_ptr<arrow::fs::FileSystem> file_system_;
    std::string root_directory_;
    std::string session_directory_;
    std::vector<CatalogTable> session_tables_;
};

} // namespace mldp_pvxs_driver::query
