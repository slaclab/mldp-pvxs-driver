//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#ifdef MLDP_PVXS_HDF5_ENABLED

#include <writer/hdf5/HDF5FilePool.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace mldp_pvxs_driver::writer;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string HDF5FilePool::safeName(const std::string& source)
{
    std::string s = source;
    for (char& c : s)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-')
        {
            c = '_';
        }
    }
    return s;
}

std::string HDF5FilePool::nowUtcFileSuffix()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return oss.str();
}

// ---------------------------------------------------------------------------
// HDF5FilePool
// ---------------------------------------------------------------------------

HDF5FilePool::HDF5FilePool(HDF5WriterConfig config)
    : config_(std::move(config))
{
    // Ensure base directory exists
    std::filesystem::create_directories(config_.basePath);
}

HDF5FilePool::~HDF5FilePool()
{
    closeAll();
}

bool HDF5FilePool::needsRotation(const FileEntry& entry) const noexcept
{
    const auto age = std::chrono::steady_clock::now() - entry.openedAt;
    if (age >= config_.maxFileAge)
    {
        return true;
    }
    const uint64_t maxBytes = config_.maxFileSizeMB * 1'048'576ULL;
    return entry.bytesWritten >= maxBytes;
}

std::shared_ptr<FileEntry> HDF5FilePool::openFile(const std::string& sourceName)
{
    const std::string fileName = safeName(sourceName) + "_" + nowUtcFileSuffix() + ".h5";
    const std::filesystem::path filePath = std::filesystem::path(config_.basePath) / fileName;

    auto entry = std::make_shared<FileEntry>();
    entry->path = filePath;
    entry->openedAt = std::chrono::steady_clock::now();
    entry->bytesWritten = 0;
    entry->file = H5::H5File(filePath.string(), H5F_ACC_TRUNC);
    return entry;
}

std::shared_ptr<FileEntry> HDF5FilePool::acquire(const std::string& sourceName)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = entries_.find(sourceName);
    if (it != entries_.end())
    {
        if (!needsRotation(*it->second))
        {
            return it->second;
        }
        // Rotate: close old (other holders keep shared_ptr alive until done)
        try { it->second->file.close(); } catch (...) {}
        entries_.erase(it);
    }
    auto entry = openFile(sourceName);
    entries_[sourceName] = entry;
    return entry;
}

void HDF5FilePool::recordWrite(const std::string& sourceName, uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = entries_.find(sourceName);
    if (it != entries_.end())
    {
        it->second->bytesWritten += bytes;
    }
}

void HDF5FilePool::flushAll() noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& [name, entry] : entries_)
    {
        try { entry->file.flush(H5F_SCOPE_LOCAL); } catch (...) {}
    }
}

void HDF5FilePool::closeAll() noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& [name, entry] : entries_)
    {
        try { entry->file.close(); } catch (...) {}
    }
    entries_.clear();
}

#endif // MLDP_PVXS_HDF5_ENABLED
