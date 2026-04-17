//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/hdf5/HDF5FilePool.h>

#include <util/log/Logger.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::log;

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
    const auto        now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm           utc{};
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
    infof("HDF5FilePool constructing — base_path={} max_file_age_s={} max_file_size_mb={} compression={}",
          config_.basePath,
          config_.maxFileAge.count(),
          config_.maxFileSizeMB,
          config_.compressionLevel);
    std::filesystem::create_directories(config_.basePath);
    debugf("HDF5FilePool base directory ready: {}", config_.basePath);
}

HDF5FilePool::~HDF5FilePool()
{
    debugf("HDF5FilePool destructor — closing all open files");
    closeAll();
}

bool HDF5FilePool::needsRotation(const FileEntry& entry) const noexcept
{
    const auto age = std::chrono::steady_clock::now() - entry.openedAt;
    if (age >= config_.maxFileAge)
    {
        debugf("HDF5FilePool rotation triggered — age threshold reached for {}", entry.path.string());
        return true;
    }
    const uint64_t maxBytes = config_.maxFileSizeMB * 1'048'576ULL;
    if (entry.bytesWritten >= maxBytes)
    {
        debugf("HDF5FilePool rotation triggered — size threshold reached for {} ({} >= {} bytes)",
               entry.path.string(), entry.bytesWritten, maxBytes);
        return true;
    }
    return false;
}

std::shared_ptr<FileEntry> HDF5FilePool::openFile(const std::string& sourceName)
{
    const std::string           fileName = safeName(sourceName) + "_" + nowUtcFileSuffix() + ".hdf5";
    const std::filesystem::path filePath = std::filesystem::path(config_.basePath) / fileName;

    infof("HDF5FilePool opening new file for source='{}' path={}", sourceName, filePath.string());
    auto entry = std::make_shared<FileEntry>();
    entry->path = filePath;
    entry->openedAt = std::chrono::steady_clock::now();
    entry->bytesWritten = 0;
    entry->file = H5::H5File(filePath.string(), H5F_ACC_TRUNC);
    debugf("HDF5FilePool file opened: {}", filePath.string());
    return entry;
}

std::shared_ptr<FileEntry> HDF5FilePool::acquire(const std::string& sourceName)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto                        it = entries_.find(sourceName);
    if (it != entries_.end())
    {
        if (!needsRotation(*it->second))
        {
            return it->second;
        }
        // Rotate: close old (other holders keep shared_ptr alive until done)
        infof("HDF5FilePool rotating file for source='{}' — closing {}", sourceName, it->second->path.string());
        try
        {
            std::lock_guard<std::mutex> fileLk(it->second->fileMutex);
            it->second->file.close();
        }
        catch (const H5::Exception& ex)
        {
            errorf("HDF5FilePool failed to close rotated file {} HDF5 error: {}", it->second->path.string(), ex.getCDetailMsg());
        }
        catch (const std::exception& ex)
        {
            errorf("HDF5FilePool failed to close rotated file {}: {}", it->second->path.string(), ex.what());
        }
        catch (...)
        {
            errorf("HDF5FilePool failed to close rotated file {} — unknown exception", it->second->path.string());
        }
        entries_.erase(it);
    }
    auto entry = openFile(sourceName);
    entries_[sourceName] = entry;
    return entry;
}

void HDF5FilePool::recordWrite(const std::string& sourceName, uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto                        it = entries_.find(sourceName);
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
        try
        {
            std::lock_guard<std::mutex> fileLk(entry->fileMutex);
            entry->file.flush(H5F_SCOPE_LOCAL);
        }
        catch (const H5::Exception& ex)
        {
            errorf("HDF5FilePool flush HDF5 error for {}: {}", entry->path.string(), ex.getCDetailMsg());
        }
        catch (const std::exception& ex)
        {
            errorf("HDF5FilePool flush failed for {}: {}", entry->path.string(), ex.what());
        }
        catch (...)
        {
            errorf("HDF5FilePool flush failed for {} — unknown exception", entry->path.string());
        }
    }
}

void HDF5FilePool::closeAll() noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    infof("HDF5FilePool closeAll — closing {} open file(s)", entries_.size());
    for (auto& [name, entry] : entries_)
    {
        try
        {
            std::lock_guard<std::mutex> fileLk(entry->fileMutex);
            entry->file.close();
            debugf("HDF5FilePool closed {}", entry->path.string());
        }
        catch (const H5::Exception& ex)
        {
            errorf("HDF5FilePool close HDF5 error for {}: {}", entry->path.string(), ex.getCDetailMsg());
        }
        catch (const std::exception& ex)
        {
            errorf("HDF5FilePool close failed for {}: {}", entry->path.string(), ex.what());
        }
        catch (...)
        {
            errorf("HDF5FilePool close failed for {} — unknown exception", entry->path.string());
        }
    }
    entries_.clear();
    debugf("HDF5FilePool all files closed");
}
