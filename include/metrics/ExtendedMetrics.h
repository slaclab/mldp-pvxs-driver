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

namespace mldp_pvxs_driver::metrics {

/**
 * @brief Generic polymorphic base for any component that registers additional
 *        Prometheus metric families into the shared registry.
 *
 * Subclass this whenever a component (writer, reader, pool, …) needs its own
 * metric families beyond those in the monolithic `Metrics` class.  All such
 * subclasses register directly into the shared `prometheus::Registry` injected
 * at construction; the registry's `Collect()` therefore includes every family
 * automatically — no changes to `Metrics` or `PeriodicMetricsDumper` are needed.
 *
 * Hierarchy:
 * @code
 *   ExtendedMetrics            ← this file
 *   └── WriterMetrics          ← include/metrics/WriterMetrics.h
 *       └── HDF5WriterMetrics  ← include/writer/hdf5/HDF5WriterMetrics.h
 * @endcode
 */
class ExtendedMetrics
{
public:
    virtual ~ExtendedMetrics() = default;
};

} // namespace mldp_pvxs_driver::metrics
