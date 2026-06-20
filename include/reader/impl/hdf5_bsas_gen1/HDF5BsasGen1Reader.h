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

#include <reader/IReader.h>
#include <reader/ReaderFactory.h>
#include <reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.h>
#include <util/bus/IDataBus.h>
#include <util/log/Logger.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1 {

/**
 * @brief Reads BSAS Gen1 HDF5 files in PyTables "fixed" format.
 *
 * Parses the block structure (block0=float64, block1=int16, block2=timestamps)
 * and emits DataBatch frames in tabular mode (one frame per column per chunk).
 */
class HDF5BsasGen1Reader : public Reader
{
public:
    HDF5BsasGen1Reader(std::shared_ptr<util::bus::IDataBus> bus,
                       std::shared_ptr<metrics::Metrics>    metrics,
                       const config::Config&                cfg);
    ~HDF5BsasGen1Reader() override;

    std::string name() const override { return config_.name(); }

private:
    struct BlockInfo
    {
        std::vector<std::string> items;
    };

    void readFile();
    void emitChunk(const std::string& sourceName,
                   const std::vector<util::bus::TimestampEntry>& timestamps,
                   const BlockInfo& block0,
                   const std::vector<double>& block0Data,
                   const BlockInfo& block1,
                   const std::vector<int16_t>& block1Data,
                   std::size_t numRows, std::size_t numFloatCols, std::size_t numIntCols);

    std::shared_ptr<util::log::ILogger> logger_;
    HDF5BsasGen1ReaderConfig config_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    REGISTER_READER("hdf5-bsas-gen1", HDF5BsasGen1Reader)
};

} // namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1
