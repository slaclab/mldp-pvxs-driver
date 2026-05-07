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
#include <writer/IWriter.h>
#include <writer/WriterFactory.h>
#include <writer/hdf5/HDF5WriterConfig.h>

#include <H5Cpp.h>

#include <memory>
#include <string>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Thin factory wrapper for the HDF5 writer.
 *
 * Selects the appropriate concrete implementation at construction time:
 * - HDF5WriterPerSource (mergeRootSources == false, default)
 * - HDF5WriterMerge    (mergeRootSources == true)
 *
 * All IWriter methods are delegated to the selected implementation.
 */
class HDF5Writer final : public IWriter
{
    REGISTER_WRITER("hdf5", HDF5Writer)
public:
    /**
     * @brief Factory constructor — parses config from the writer.hdf5 YAML sub-node.
     */
    explicit HDF5Writer(const config::Config&             node,
                        std::shared_ptr<metrics::Metrics> metrics = nullptr);

    /**
     * @brief Typed constructor — for direct use and unit tests.
     */
    explicit HDF5Writer(HDF5WriterConfig config);

    ~HDF5Writer() override = default;

    std::string name() const override;
    void        start() override;
    bool        push(util::bus::IDataBus::EventBatch batch) noexcept override;
    void        stop() noexcept override;

    bool supports_multi_root_source() const noexcept override
    {
        return true;
    }

private:
    std::unique_ptr<IWriter> impl_;
};

} // namespace mldp_pvxs_driver::writer
