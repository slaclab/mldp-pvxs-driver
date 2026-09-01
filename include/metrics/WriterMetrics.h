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

#include <metrics/ExtendedMetrics.h>

namespace mldp_pvxs_driver::metrics {

/**
 * @brief Abstract base for per-writer metric collections.
 *
 * Extends @ref ExtendedMetrics — the shared root for all component-specific
 * metric extensions.  Each writer implementation subclasses WriterMetrics and
 * registers its own metric families into the shared Prometheus registry.
 */
class WriterMetrics : public ExtendedMetrics
{
public:
    ~WriterMetrics() override = default;
};

} // namespace mldp_pvxs_driver::metrics
