//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////


/** @file QueryTableCatalog.h
 * @brief Owns IPC-backed query tables created during a session. */
#pragma once

#include <arrow/filesystem/filesystem.h>
#include <arrow/record_batch.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::query {

class IRecordBatchStream;

/** @brief Lifetime of an IPC-backed table stored by the query catalog. */
enum class TableLifetime {
    Session,    ///< Table is removed when the session ends.
    Persistent  ///< Table survives across REPL sessions.
};

/** @brief Stored table metadata and the path to its Arrow IPC snapshot. */
struct CatalogTable {
    std::string                    name;                              ///< Logical table name used in SQL.
    TableLifetime                  lifetime{TableLifetime::Session};  ///< Whether this table is session-scoped or persistent.
    std::string                    path;                              ///< Absolute path to the Arrow IPC snapshot file.
    std::shared_ptr<arrow::Schema> schema;                            ///< Schema of the stored batches.
    int64_t                        row_count{0};                      ///< Total rows in the snapshot.
    int64_t                        byte_count{0};                     ///< Total bytes in the snapshot file.
};

/** @brief Owns Arrow IPC snapshots created by CREATE [TEMP] TABLE statements during a query session. */
class QueryTableCatalog
{
public:
    /** @brief Constructs a catalog rooted at the given directory.
     *  @param[in] file_system       File system for reading and writing snapshots.
     *  @param[in] root_directory    Root directory for persistent and session-scoped tables. */
    QueryTableCatalog(std::shared_ptr<arrow::fs::FileSystem> file_system, std::string root_directory);
    ~QueryTableCatalog();

    QueryTableCatalog(const QueryTableCatalog&) = delete;
    QueryTableCatalog& operator=(const QueryTableCatalog&) = delete;

    /** @brief Creates a new catalog table from a batch vector.
     *  @param[in] name     Logical table name.
     *  @param[in] lifetime Session or persistent lifetime.
     *  @param[in] batches  Batches to snapshot.
     *  @return Arrow Status. */
    arrow::Status create(std::string name, TableLifetime lifetime,
                         const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);

    /** @brief Drains a pull stream into one Arrow IPC snapshot.
     *  @param[in] name     Logical table name.
     *  @param[in] lifetime Session or persistent lifetime.
     *  @param[in] stream   Batch stream to drain.
     *  @return Arrow Status. */
    arrow::Status create(std::string name, TableLifetime lifetime, IRecordBatchStream& stream);

    /** @brief Removes a catalog table and its snapshot file.
     *  @param[in] name Table name.
     *  @return Arrow Status. */
    arrow::Status drop(const std::string& name);

    /** @brief Looks up a table by name.
     *  @param[in] name Table name.
     *  @return The CatalogTable if found, or std::nullopt. */
    [[nodiscard]] std::optional<CatalogTable> find(const std::string& name) const;

    /** @brief Returns all registered catalog tables.
     *  @return Vector of all tables. */
    [[nodiscard]] std::vector<CatalogTable> tables() const;

    /** @brief Returns the names of all registered catalog tables.
     *  @return Vector of table names. */
    [[nodiscard]] std::vector<std::string> tableNames() const;

    /** @brief Reads all batches from a catalog table's snapshot.
     *  @param[in] table Catalog table metadata.
     *  @return Arrow Result with the batch vector, or an error status. */
    arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> read(const CatalogTable& table) const;

    /** @brief Removes all session-scoped tables and their snapshot files.
     *  @return Arrow Status. */
    arrow::Status cleanupSession();

private:
    std::shared_ptr<arrow::fs::FileSystem> file_system_;       ///< File system for I/O.
    std::string                            root_directory_;    ///< Root directory for catalog files.
    std::string                            session_directory_; ///< Subdirectory for session-scoped tables.
    std::vector<CatalogTable>              session_tables_;    ///< Registered tables (both session and persistent).
};

} // namespace mldp_pvxs_driver::query
