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
    explicit HDF5FilePool(HDF5WriterConfig config);
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
    const HDF5WriterConfig                                      config_;
    mutable std::mutex                                          mutex_;
    std::unordered_map<std::string, std::shared_ptr<FileEntry>> entries_;

    bool                       needsRotation(const FileEntry& entry) const noexcept;
    std::shared_ptr<FileEntry> openFile(const std::string& sourceName);
    static std::string         safeName(const std::string& source);
    static std::string         nowUtcFileSuffix();
};

} // namespace mldp_pvxs_driver::writer
