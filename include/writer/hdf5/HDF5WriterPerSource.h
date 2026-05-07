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

#include <writer/hdf5/HDF5WriterBase.h>
#include <writer/hdf5/HDF5FilePool.h>
#include <writer/WriterFactory.h>
#include <config/Config.h>
#include <metrics/Metrics.h>

#include <memory>

namespace mldp_pvxs_driver::writer {

/**
 * @brief HDF5 writer — non-merge mode (one file per root_source).
 *
 * Uses HDF5FilePool to manage per-source file handles and rotation.
 */
class HDF5WriterPerSource final : public HDF5WriterBase
{
    REGISTER_WRITER("hdf5", HDF5WriterPerSource)
public:
    explicit HDF5WriterPerSource(const config::Config&             node,
                                  std::shared_ptr<metrics::Metrics> metrics = nullptr);
    explicit HDF5WriterPerSource(HDF5WriterConfig                    config,
                                  std::shared_ptr<metrics::Metrics>   metrics = nullptr);
    ~HDF5WriterPerSource() override;

protected:
    void writeFrameImpl(const std::string&          source,
                         const util::bus::DataBatch& frame,
                         uint64_t                    batchSeq) override;

    void flushTabularBufferImpl(const std::string& source,
                                 TabularBuffer&     buf) override;

    void doFlushAll() noexcept override;
    void doStart() override;
    void doStop() noexcept override;

private:
    std::unique_ptr<HDF5FilePool> pool_;

    void appendFrame(const std::string&          sourceName,
                     const util::bus::DataBatch& batch,
                     H5::H5File&                 file,
                     uint64_t                    batchSeq);
};

} // namespace mldp_pvxs_driver::writer
