//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// Reader.hpp
#pragma once

#include <memory>
#include <string>

namespace mldp_pvxs_driver::metrics {
class Metrics;
}

namespace mldp_pvxs_driver::util::bus {
class IDataBus;
}

namespace mldp_pvxs_driver::reader {

using ReaderUPtr = std::unique_ptr<class Reader>;

/**
 * @brief Contract for reader implementations that sample data and forward events to the bus.
 *
 * Derived classes register with @c ReaderFactory and implement sensor-specific polling logic.
 */
class Reader
{
public:
    /** @brief Construct the reader with the event bus connection that will receive updates. */
    Reader(std::shared_ptr<util::bus::IDataBus> bus,
           std::shared_ptr<metrics::Metrics>    metrics = nullptr)
        : bus_(std::move(bus))
        , metrics_(std::move(metrics)) {}

    virtual ~Reader() = default;

    /** @brief Return the human-readable identifier for this reader instance. */
    virtual std::string name() const = 0;

protected:
    /** @brief Event bus that derived readers use to deliver events.
     *
     * Ownership is shared so readers can outlive their producers.
     */
    std::shared_ptr<util::bus::IDataBus> bus_;
    /** @brief Shared metrics collector (may be null when not configured). */
    std::shared_ptr<metrics::Metrics> metrics_;
};

} // namespace mldp_pvxs_driver::reader
