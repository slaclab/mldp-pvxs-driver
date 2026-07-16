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

#ifdef BUILD_PYTHON_PROCESSOR

#include <BS_thread_pool.hpp>
#include <metrics/Metrics.h>
#include <processor/IChannelProcessor.h>
#include <util/bus/IDataBus.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace mldp_pvxs_driver::processor {

class PythonScriptDirectoryLoader
{
public:
    /** Load every Python processor module in a directory. */
    static std::vector<IChannelProcessorUPtr> load(
        const std::filesystem::path&           script_dir,
        std::shared_ptr<util::bus::IDataBus>   bus,
        std::shared_ptr<metrics::Metrics>      metrics,
        std::shared_ptr<BS::light_thread_pool> thread_pool = nullptr);

    /** Load exactly one Python processor module. */
    static std::vector<IChannelProcessorUPtr> loadScript(
        const std::filesystem::path&           script_path,
        std::shared_ptr<util::bus::IDataBus>   bus,
        std::shared_ptr<metrics::Metrics>      metrics,
        std::shared_ptr<BS::light_thread_pool> thread_pool = nullptr);
};

} // namespace mldp_pvxs_driver::processor

#endif // BUILD_PYTHON_PROCESSOR
