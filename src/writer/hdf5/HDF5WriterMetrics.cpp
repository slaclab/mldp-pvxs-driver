//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/hdf5/HDF5WriterMetrics.h>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

using namespace mldp_pvxs_driver::metrics;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HDF5WriterMetrics::HDF5WriterMetrics(prometheus::Registry& registry,
                                     const std::string&    controller_name,
                                     const std::string&    writer_name)
{
    const prometheus::Labels clabels{{"controller", controller_name},
                                     {"writer", writer_name}};

    batches_written_family_ = &prometheus::BuildCounter()
                                   .Name("mldp_pvxs_driver_hdf5_batches_written_total")
                                   .Help("Total EventBatches written to HDF5.")
                                   .Labels(clabels)
                                   .Register(registry);

    rows_written_family_ = &prometheus::BuildCounter()
                                .Name("mldp_pvxs_driver_hdf5_rows_written_total")
                                .Help("Total rows (samples) appended to HDF5 datasets, per source.")
                                .Labels(clabels)
                                .Register(registry);

    bytes_written_family_ = &prometheus::BuildCounter()
                                 .Name("mldp_pvxs_driver_hdf5_bytes_written_total")
                                 .Help("Total bytes written to HDF5 files, per source.")
                                 .Labels(clabels)
                                 .Register(registry);

    queue_depth_family_ = &prometheus::BuildGauge()
                               .Name("mldp_pvxs_driver_hdf5_queue_depth")
                               .Help("Current depth of the HDF5 write queue.")
                               .Labels(clabels)
                               .Register(registry);

    queue_drops_family_ = &prometheus::BuildCounter()
                               .Name("mldp_pvxs_driver_hdf5_queue_drops_total")
                               .Help("Total EventBatches dropped due to write-queue overflow.")
                               .Labels(clabels)
                               .Register(registry);

    file_rotations_family_ = &prometheus::BuildCounter()
                                  .Name("mldp_pvxs_driver_hdf5_file_rotations_total")
                                  .Help("Total HDF5 file rotations triggered by age or size threshold, per source.")
                                  .Labels(clabels)
                                  .Register(registry);

    write_latency_ms_buckets_ = {
        0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0};

    write_latency_ms_family_ = &prometheus::BuildHistogram()
                                    .Name("mldp_pvxs_driver_hdf5_write_latency_ms")
                                    .Help("Latency of HDF5 appendFrame / flushTabularBuffer calls (milliseconds).")
                                    .Labels(clabels)
                                    .Register(registry);
}

// ---------------------------------------------------------------------------
// Writer-level methods
// ---------------------------------------------------------------------------

void HDF5WriterMetrics::incrementBatchesWritten(double value)
{
    batches_written_family_->Add({}).Increment(value);
}

void HDF5WriterMetrics::setQueueDepth(double value)
{
    queue_depth_family_->Add({}).Set(value);
}

void HDF5WriterMetrics::incrementQueueDrops(double value)
{
    queue_drops_family_->Add({}).Increment(value);
}

void HDF5WriterMetrics::observeWriteLatencyMs(double ms)
{
    write_latency_ms_family_->Add({}, write_latency_ms_buckets_).Observe(ms);
}

// ---------------------------------------------------------------------------
// Per-source methods
// ---------------------------------------------------------------------------

void HDF5WriterMetrics::incrementRowsWritten(const std::string& source, double value)
{
    rows_written_family_->Add({{"source", source}}).Increment(value);
}

void HDF5WriterMetrics::incrementBytesWritten(const std::string& source, double value)
{
    bytes_written_family_->Add({{"source", source}}).Increment(value);
}

void HDF5WriterMetrics::incrementFileRotations(const std::string& source, double value)
{
    file_rotations_family_->Add({{"source", source}}).Increment(value);
}
