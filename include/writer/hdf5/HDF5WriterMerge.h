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

#include <config/Config.h>
#include <metrics/Metrics.h>
#include <writer/WriterFactory.h>
#include <writer/hdf5/HDF5WriterBase.h>

#include <H5Cpp.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace mldp_pvxs_driver::writer {

/**
 * @brief HDF5 writer — merge mode (all sources share one file).
 *
 * Each root_source gets its own HDF5 group (/<source_name>/).
 * The file is rotated when age or size thresholds are crossed.
 */
class HDF5WriterMerge final : public HDF5WriterBase
{
    REGISTER_WRITER("hdf5-merge", HDF5WriterMerge)
public:
    explicit HDF5WriterMerge(const config::Config&             node,
                             std::shared_ptr<metrics::Metrics> metrics = nullptr);
    explicit HDF5WriterMerge(HDF5WriterConfig                  config,
                             std::shared_ptr<metrics::Metrics> metrics = nullptr);
    ~HDF5WriterMerge() override;

protected:
    std::size_t writeFrameImpl(const std::string&          source,
                               const util::bus::DataBatch& frame,
                               uint64_t                    batchSeq) override;

    void flushTabularBufferImpl(const std::string& source,
                                TabularBuffer&     buf) override;

    void doFlushAll() noexcept override;
    void onHDF5Start() override;
    void onHDF5Stop() noexcept override;

private:
    std::unique_ptr<H5::H5File>           mergeFile_;
    std::filesystem::path                 mergePath_;
    std::filesystem::path                 mergeFinalPath_;
    mutable std::mutex                    mergeFileMutex_;
    std::set<std::string>                 mergeOpenGroups_;
    uint64_t                              mergeBytesWritten_{0};
    std::chrono::steady_clock::time_point mergeFileOpenedAt_;
    std::atomic<bool>                     mergeRotating_{false};
    uint64_t                              mergeFileSeq_{0};

    void openMergeFile();
    void closeMergeFile() noexcept;
    void rotateMergeFile();
    void ensureMergeGroup(const std::string& sourceName); // caller MUST hold mergeFileMutex_
    void checkMergeRotation();
    void appendFrameMerge(const std::string&          sourceName,
                          const util::bus::DataBatch& batch,
                          uint64_t                    batchSeq);
    void flushTabularBufferMerge(const std::string& sourceName,
                                 TabularBuffer&     buf);
};

} // namespace mldp_pvxs_driver::writer
