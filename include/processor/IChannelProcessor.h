//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * @file IChannelProcessor.h
 * @brief Writer-compatible interface for algorithm-backed virtual channel processors.
 */

#pragma once

#include <writer/IWriter.h>

#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

/**
 * @class IChannelProcessor
 * @brief Runtime interface used by the controller for processor-backed virtual writers.
 * @details
 * Channel processors consume one set of input sources and may emit one or more
 * virtual output sources back onto the bus while exposing the writer lifecycle
 * expected by the controller.
 */
class IChannelProcessor : public writer::IWriter
{
public:
    ~IChannelProcessor() override = default;

    /** @brief Reader name stamped onto emitted event batches. */
    virtual const std::string& outputReaderName() const noexcept = 0;

    /** @brief Virtual output root-source names this processor may emit. */
    virtual std::vector<std::string> outputSourceNames() const noexcept = 0;

    /** @brief Ordered input root-source names this processor consumes. */
    virtual const std::vector<std::string>& inputSourceNames() const noexcept = 0;
};

using IChannelProcessorUPtr = std::unique_ptr<IChannelProcessor>;

} // namespace mldp_pvxs_driver::processor
