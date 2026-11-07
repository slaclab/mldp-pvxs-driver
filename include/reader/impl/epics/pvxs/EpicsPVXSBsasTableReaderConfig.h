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
 * @file EpicsPVXSBsasTableReaderConfig.h
 * @brief Configuration for the `epics-pvxs-bsas-gen1` / `epics-pvxs-bsas-gen2` readers.
 */

#pragma once

#include <reader/impl/epics/pvxs/EpicsPVXSReaderConfig.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mldp_pvxs_driver::reader::impl::epics {

inline constexpr char RowsPerDataBatchKey[] = "rows-per-data-batch";
inline constexpr char PvsPerDataBatchKey[] = "pvs-per-data-batch";
inline constexpr char DataBatchesPerPushKey[] = "data-batches-per-push";
inline constexpr char PidFieldKey[] = "pidField";
inline constexpr char ValueSuffixKey[] = "valueSuffix";

/**
 * @brief Typed view over a BSAS table reader entry.
 *
 * Extends EpicsPVXSReaderConfig (PVs, thread pool, metadata, PVXS client
 * settings) with table settings.  Each setting can be given at reader level
 * and overridden per PV under `option`:
 *
 * @code{.yaml}
 * readers:
 *   - epics-pvxs-bsas-gen2:
 *       - name: bsas_gen2
 *         rows-per-data-batch: 120     # rows per DataBatch; 0 = all rows
 *         pvs-per-data-batch: 1        # PV columns per DataBatch; 0 = all
 *         data-batches-per-push: 50    # DataBatches per bus EventBatch; 0 = all
 *         tsSeconds: secondsPastEpoch
 *         tsNanos: nanoseconds
 *         pidField: pid                # Gen2 only; column always skipped
 *         valueSuffix: Val             # Gen2 only; case-insensitive
 *         pvs:
 *           - name: "BSAS:GEN2:TABLE"
 *             option:
 *               pvs-per-data-batch: 0
 * @endcode
 *
 * `option.type` is rejected: the table generation is implied by the reader type.
 * `column-batch-size` is rejected: use `data-batches-per-push`.
 */
class EpicsPVXSBsasTableReaderConfig : public EpicsPVXSReaderConfig
{
public:
    /** @brief BSAS table generation handled by the reader. */
    enum class Generation
    {
        Gen1, ///< Every non-timestamp column is a PV value column.
        Gen2, ///< `<PV>.Val` columns only; pid and statistics are dropped.
    };

    /** @brief Effective table settings for one PV. */
    struct TableOptions
    {
        std::size_t rowsPerDataBatch = 120;               ///< Rows per DataBatch (0 = all rows).
        std::size_t pvsPerDataBatch = 1;                  ///< PV columns per DataBatch (0 = all).
        std::size_t dataBatchesPerPush = 50;              ///< DataBatches per bus EventBatch (0 = all).
        std::string tsSecondsField = "secondsPastEpoch"; ///< Per-row epoch seconds column.
        std::string tsNanosField = "nanoseconds";        ///< Per-row nanoseconds column.
        std::string pidField = "pid";                    ///< Gen2 pulse ID column (skipped).
        std::string valueSuffix = "Val";                 ///< Gen2 value column suffix (case-insensitive).
    };

    EpicsPVXSBsasTableReaderConfig() = default;

    /**
     * @brief Parse and validate a table reader entry.
     *
     * @param readerEntry Reader YAML node.
     * @param generation Table generation (selects the accepted keys).
     * @param readerType Reader type name used in error messages.
     * @throws EpicsReaderConfig::Error on invalid configuration.
     */
    EpicsPVXSBsasTableReaderConfig(const ::mldp_pvxs_driver::config::Config& readerEntry,
                                   Generation                                generation,
                                   std::string_view                          readerType);

    /** @return Table generation. */
    Generation generation() const
    {
        return generation_;
    }

    /** @return Reader-level table settings (defaults for every PV). */
    const TableOptions& defaultOptions() const
    {
        return defaults_;
    }

    /** @return Effective settings for @p pvName (reader defaults when unknown). */
    const TableOptions& optionsFor(const std::string& pvName) const;

private:
    Generation                                    generation_ = Generation::Gen1;
    TableOptions                                  defaults_;
    std::unordered_map<std::string, TableOptions> per_pv_;
};

} // namespace mldp_pvxs_driver::reader::impl::epics
