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

#ifdef MLDP_PVXS_HDF5_ENABLED

#include <config/Config.h>
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/hdf5/HDF5FilePool.h>
#include <writer/hdf5/HDF5WriterConfig.h>

#include <H5Cpp.h>
#include <common.pb.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Writer that stores incoming event batches as HDF5 datasets.
 *
 * One HDF5 file per `root_source` is maintained by an `HDF5FilePool`;
 * file rotation is driven by age and size thresholds in `HDF5WriterConfig`.
 *
 * Internal threading:
 * - **Writer thread**: drains the bounded MPSC queue and calls `appendFrame()`.
 * - **Flush thread**: calls `HDF5FilePool::flushAll()` every `flush-interval-ms`.
 *
 * HDF5 layout inside each source file:
 * @code
 * / (root)
 * ├── timestamps          int64   ns-since-epoch   shape=(N,) unlimited+chunked
 * ├── <col_name_0>        …       per DataFrame col
 * └── …
 * @endcode
 */
class HDF5Writer final : public IWriter {
    REGISTER_WRITER("hdf5", HDF5Writer)
public:
    /**
     * @brief Factory constructor — parses config from the writer.hdf5 YAML sub-node.
     *
     * Called by the @ref WriterFactory registry. The @p metrics parameter is
     * accepted for interface uniformity but is not used by the HDF5 writer.
     */
    explicit HDF5Writer(const config::Config&             node,
                        std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit HDF5Writer(HDF5WriterConfig config);
    ~HDF5Writer() override;

    std::string name() const override { return "hdf5"; }
    void start() override;
    bool push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void stop() noexcept override;

private:
    using EventBatch = util::bus::IDataBus::EventBatch;

    static constexpr std::size_t kQueueCapacity = 8192;

    HDF5WriterConfig             config_;
    std::unique_ptr<HDF5FilePool> pool_;

    // Queue
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;
    std::deque<EventBatch>  queue_;
    std::atomic<bool>       stopping_{false};

    // Worker threads
    std::thread writerThread_;
    std::thread flushThread_;

    void writerLoop();
    void flushLoop();

    void appendFrame(const std::string&                    sourceName,
                     const dp::service::common::DataFrame& frame,
                     H5::H5File&                           file);

    H5::DataSet ensureDataset(H5::H5File&        file,
                              const std::string& name,
                              const H5::DataType& dtype);
};

} // namespace mldp_pvxs_driver::writer

#endif // MLDP_PVXS_HDF5_ENABLED
