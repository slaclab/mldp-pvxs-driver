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

#include <writer/hdf5/HDF5WriterConfig.h>

#include <H5Cpp.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::writer {

/**
 * @brief RAII entry describing one open HDF5 file for a single data source.
 */
struct FileEntry
{
    H5::H5File                            file;            ///< Open HDF5 file handle.
    std::filesystem::path                 path;            ///< Absolute path on disk.
    std::chrono::steady_clock::time_point openedAt;        ///< When the file was opened.
    uint64_t                              bytesWritten{0}; ///< Cumulative bytes written since open.
    mutable std::mutex                    fileMutex;       ///< Guards all access to file (HDF5 is not thread-safe).
};

/**
 * @brief Per-source HDF5 file handle pool.
 *
 * Mirrors `MLDPGrpcIngestionePool` from `include/pool/`.  One HDF5 file is
 * kept open per `root_source` name; files are rotated (closed and a new one
 * opened) when either the configured age or size threshold is exceeded.
 *
 * Thread-safety:
 * - The pool mutex guards the map (lookup / rotation / insertion).
 * - Each `FileEntry` owns a `fileMutex` that must be held for any HDF5 I/O
 *   on that entry's `file` handle.  This prevents concurrent access from the
 *   writer thread (appendFrame) and the flush thread (flushAll), which would
 *   otherwise corrupt the HDF5 metadata cache.
 * - Multiple writers can perform concurrent I/O on **different** sources
 *   without contention (each source has its own `fileMutex`).
 *
 * File naming convention:
 * @code
 * <base-path>/<safe_source>_<YYYYMMDDTHHMMSSz>.h5
 * @endcode
 * where `:` and other characters outside `[A-Za-z0-9._-]` are replaced by `_`.
 */
class HDF5FilePool
{
public:
    /**
     * @brief Construct the pool with the given writer configuration.
     *
     * Does not open any files; files are opened lazily on the first call to
     * acquire() for each source.
     *
     * @param config  Validated writer configuration (basePath, rotation thresholds, etc.).
     */
    explicit HDF5FilePool(HDF5WriterConfig config);

    /**
     * @brief Destructor — calls closeAll() to flush and close every open file.
     *
     * Safe to call while no other thread is using the pool (caller must ensure
     * acquire() / recordWrite() / flushAll() have quiesced first).
     */
    ~HDF5FilePool();

    /**
     * @brief Acquire (or create / rotate) the file handle for @p sourceName.
     *
     * The mutex is held only for the lookup and optional rotation; the returned
     * `shared_ptr` is safe to use without the lock for HDF5 I/O.
     *
     * @param sourceName Root source identifier (e.g. PV name).
     * @return Shared ownership of the current `FileEntry` for this source.
     */
    std::shared_ptr<FileEntry> acquire(const std::string& sourceName);

    /**
     * @brief Record that @p bytes have been appended for @p sourceName.
     *
     * Updates the `bytesWritten` counter so rotation logic can check size.
     *
     * @param sourceName  Source whose entry is updated.
     * @param bytes       Number of bytes written in the last operation.
     */
    void recordWrite(const std::string& sourceName, uint64_t bytes);

    /**
     * @brief Flush all open file handles (called by the flush thread).
     */
    void flushAll() noexcept;

    /**
     * @brief Close all open file handles (called on writer stop).
     */
    void closeAll() noexcept;

private:
    const HDF5WriterConfig                                      config_;   ///< Immutable pool configuration.
    mutable std::mutex                                          mutex_;    ///< Guards entries_ map (lookup, insert, rotate).
    std::unordered_map<std::string, std::shared_ptr<FileEntry>> entries_;  ///< sourceName → open FileEntry.

    /**
     * @brief Return true if @p entry has exceeded the configured age or size limit.
     *
     * Called under mutex_ during acquire() to decide whether to rotate.
     * Checks:
     * - `steady_clock::now() - entry.openedAt >= config_.maxFileAge`
     * - `entry.bytesWritten >= config_.maxFileSizeMB * 1024 * 1024`
     *
     * @param entry  FileEntry to evaluate (must not be concurrently modified).
     * @return true if the file should be closed and replaced with a new one.
     */
    bool needsRotation(const FileEntry& entry) const noexcept;

    /**
     * @brief Open a new HDF5 file for @p sourceName and insert it into entries_.
     *
     * File path is:
     * @code
     * config_.basePath / safeName(sourceName) + "_" + nowUtcFileSuffix() + ".h5"
     * @endcode
     * The file is created with H5F_ACC_TRUNC (truncate/create).  If the base
     * directory does not exist it is created recursively.
     *
     * @param sourceName  Root source identifier; used to build the file name.
     * @return Shared pointer to the newly created and inserted FileEntry.
     * @throws H5::FileIException if the HDF5 library cannot create the file.
     */
    std::shared_ptr<FileEntry> openFile(const std::string& sourceName);

    /**
     * @brief Sanitise @p source for use as a file-name component.
     *
     * Replaces every character outside `[A-Za-z0-9._-]` with `_`.
     * This prevents path-separator injection and ensures the resulting
     * filename is valid on all POSIX and NTFS file systems.
     *
     * @param source  Raw source/PV name (may contain `:`, `/`, spaces, etc.).
     * @return Sanitised copy safe for embedding in a file path.
     */
    static std::string safeName(const std::string& source);

    /**
     * @brief Return a UTC timestamp string suitable for use in file names.
     *
     * Format: `YYYYMMDDTHHMMSSz`  (ISO 8601 basic, UTC, 'z' suffix).
     * Example: `20240315T142233z`
     *
     * @return Timestamp string based on the current UTC wall-clock time.
     */
    static std::string nowUtcFileSuffix();
};

} // namespace mldp_pvxs_driver::writer
