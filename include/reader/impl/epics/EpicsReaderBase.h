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
 * @file EpicsReaderBase.h
 * @brief Abstract base class for EPICS reader implementations.
 *
 * This header defines EpicsReaderBase, which provides common functionality
 * shared by all EPICS reader implementations (EpicsPVXSReader, EpicsBaseReader).
 * It handles configuration parsing, runtime PV configuration, thread pool
 * management, and event bus integration.
 */

#pragma once

#include <reader/Reader.h>
#include <reader/impl/epics/EpicsReaderConfig.h>
#include <util/bus/IDataBus.h>
#include <util/log/Logger.h>

#include <BS_thread_pool.hpp>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::reader::impl::epics {

/**
 * @brief Type alias for a set of PV names.
 *
 * Uses std::set to ensure unique PV names and consistent ordering.
 */
using PVSet = std::set<std::string>;

/**
 * @brief Abstract base class for EPICS reader implementations.
 *
 * This class provides common infrastructure for EPICS readers, including:
 * - Configuration parsing and validation
 * - Per-PV runtime configuration (processing mode, timestamp fields)
 * - Thread pool for parallel event processing
 * - Event bus integration for publishing converted data
 * - Metrics and logging infrastructure
 *
 * Derived classes (EpicsPVXSReader, EpicsBaseReader) implement the actual
 * EPICS library integration and event processing logic.
 *
 * @note This class is not instantiable directly; use one of the concrete
 *       implementations registered with the ReaderFactory.
 *
 * @see EpicsPVXSReader for PVXS-based implementation
 * @see EpicsBaseReader for EPICS Base pvaClient implementation
 */
class EpicsReaderBase : public reader::Reader
{
public:
    /**
     * @brief Construct the base reader with common dependencies.
     *
     * Initializes the thread pool, parses PV configurations, and sets up
     * runtime configuration for each monitored PV.
     *
     * @param bus Event bus for publishing converted PV data.
     * @param metrics Metrics collector for instrumentation (may be null).
     * @param cfg Parsed reader configuration.
     * @param logger Logger instance for diagnostic output.
     */
    EpicsReaderBase(std::shared_ptr<util::bus::IDataBus> bus,
                    std::shared_ptr<metrics::Metrics>         metrics,
                    const EpicsReaderConfig&                  cfg,
                    std::shared_ptr<util::log::ILogger>       logger);

    /**
     * @brief Virtual destructor - stops the thread pool.
     *
     * Signals the running flag to false and waits for the thread pool
     * to complete any pending tasks.
     */
    ~EpicsReaderBase() override;

    /**
     * @brief Get the reader's configured name.
     *
     * @return The name specified in the reader's YAML configuration.
     */
    std::string name() const override
    {
        return name_;
    }

protected:
    /**
     * @brief Runtime configuration for a single PV.
     *
     * Specifies how the reader should process updates from this PV,
     * including the processing mode and any mode-specific parameters.
     */
    struct PVRuntimeConfig
    {
        /**
         * @brief Processing mode for PV updates.
         */
        /**
         * @brief Processing mode for PV updates.
         */
        enum class Mode
        {
            Default,       ///< Standard scalar/array processing with structure timestamp.
            SlacBsasTable, ///< SLAC BSAS NTTable with per-row timestamps.
        };

        Mode        mode = Mode::Default; ///< Selected processing mode.
        std::string tsSecondsField;       ///< Column name for epoch seconds (SlacBsasTable mode).
        std::string tsNanosField;         ///< Column name for nanoseconds (SlacBsasTable mode).
    };

    /**
     * @brief Look up runtime configuration for a PV.
     *
     * @param pvName The PV name to look up.
     * @return Pointer to the PV's runtime configuration, or nullptr if
     *         no specific configuration exists (use default processing).
     */
    const PVRuntimeConfig* runtimeConfigFor(const std::string& pvName) const;

    /**
     * @brief Get the set of configured PV names.
     *
     * @return Const reference to the set of PV names from configuration.
     */
    const PVSet& pvNames() const
    {
        return pvNames_;
    }

    std::shared_ptr<util::log::ILogger>              logger_;          ///< Logger instance.
    EpicsReaderConfig                                config_;          ///< Parsed reader configuration.
    std::string                                      name_;            ///< Reader name from configuration.
    std::atomic<bool>                                running_{true};   ///< Flag indicating reader is active.
    std::shared_ptr<BS::light_thread_pool>           reader_pool_;     ///< Thread pool for event processing.
    std::unordered_map<std::string, PVRuntimeConfig> pvRuntimeByName_; ///< Per-PV runtime configuration.
    PVSet                                            pvNames_;         ///< Set of monitored PV names.
};

} // namespace mldp_pvxs_driver::reader::impl::epics
