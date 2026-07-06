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
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1 {

/**
 * @brief Reads BSAS Gen1 HDF5 files in flat format.
 *
 * Resolves a glob pattern to one or more HDF5 files and processes each
 * sequentially. Each file contains root-level 1-D datasets: float64 and
 * int16 data columns alongside secondsPastEpoch/nanoseconds timestamp
 * vectors. Data is read in configurable row-chunks and emitted as
 * TimeSeriesPayload frames in tabular mode (one frame per column per chunk).
 *
 * Per-file Prometheus metrics are recorded using the file name (not the
 * configured reader name) as the @c source label, enabling per-file
 * observability when a glob matches multiple files.
 */
class HDF5BsasGen1Reader : public Reader
{
public:
    /**
     * @brief Construct and immediately begin background reading.
     * @param bus    Data bus to push EventBatch payloads onto.
     * @param metrics Prometheus metric registry (may be nullptr in tests).
     * @param cfg    Parsed YAML configuration block for this reader instance.
     */
    HDF5BsasGen1Reader(std::shared_ptr<util::bus::IDataBus> bus,
                       std::shared_ptr<metrics::Metrics>    metrics,
                       const config::Config&                cfg);
    ~HDF5BsasGen1Reader() override;

    std::string name() const override { return config_.name(); }

private:
    /** @brief Column descriptor discovered during HDF5 dataset enumeration. */
    struct ColumnInfo
    {
        std::string name;  ///< Dataset name within the HDF5 file (used as column name).
        std::unordered_map<std::string, std::string> metadata; ///< All HDF5 attributes as key/value.
        enum class Type { Float64, Int16 } type; ///< Numeric storage type.
    };

    /**
     * @brief Worker entry-point: resolves glob, iterates files, reads chunks.
     *
     * Runs on a dedicated thread spawned in the constructor. Processes all
     * matched files sequentially and calls signalCompleted() on exit.
     *
     * @note Backpressure guarantee: as long as the reader is NOT stopped by
     * the controller, data is never discarded. When the downstream bus is full,
     * the reader waits (with retry) until space becomes available — it will
     * never drop a chunk. Only when the controller sets running_ to false does
     * the reader bail out immediately, skipping any unprocessed data without
     * pushing it onto the bus.
     */
    void readFile();

    /**
     * @brief Assemble and push one chunk's worth of data onto the bus.
     *
     * Uses a retry-with-backoff strategy: if the bus rejects the push
     * (queue full / backpressure), waits 10 ms and retries once.
     *
     * @note No-discard guarantee: until the reader is stopped by the
     * controller, this method will always wait and retry rather than
     * discarding data. A chunk is either delivered successfully or the
     * reader exits because the controller requested shutdown — data is
     * never silently dropped while the reader is active.
     *
     * @param sourceName  Logical source identifier (config_.name()).
     * @param currentFile Full filesystem path of the file being read.
     * @param timestamps  Row timestamps for this chunk.
     * @param columns     Resolved column descriptors (name/label/type).
     * @param floatData   Contiguous float64 column data (column-major).
     * @param intData     Contiguous int16 column data (column-major).
     * @param numRows     Number of rows in this chunk.
     * @param numFloatCols Number of float64 columns.
     * @param numIntCols  Number of int16 columns.
     * @return true if both data batch and marker were pushed successfully;
     *         false if the reader was stopped or retry failed (caller should
     *         abort the read loop).
     */
    bool emitChunk(const std::string& sourceName,
                   const std::string& currentFile,
                   const std::vector<util::bus::TimestampEntry>& timestamps,
                   const std::vector<ColumnInfo>& columns,
                   const std::vector<double>& floatData,
                   const std::vector<int16_t>& intData,
                   std::size_t numRows, std::size_t numFloatCols, std::size_t numIntCols);

    std::shared_ptr<util::log::ILogger> logger_;
    HDF5BsasGen1ReaderConfig config_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::unordered_map<std::string, uint16_t> pv_shard_slot_map_;
    std::size_t                               next_shard_{0};
    std::mt19937                              rng_;

    REGISTER_READER("hdf5-bsas-gen1", HDF5BsasGen1Reader)
};

} // namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1
