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
 * @file EchoAlgorithm.cpp
 * @brief Implements the optional pass-through processor algorithm used for pipeline smoke tests.
 */

#include <processor/impl/EchoAlgorithm.h>

#ifdef BUILD_ECHO_PROCESSOR

#include <processor/ChannelProcessorFactory.h>
#include <util/log/Logger.h>

#include <type_traits>

namespace mldp_pvxs_driver::processor {

namespace {

template <typename T>
constexpr bool is_vector_v = false;

template <typename T, typename Alloc>
constexpr bool is_vector_v<std::vector<T, Alloc>> = true;

/**
 * @brief Emit debug logging for one echoed column value container.
 * @param[in] output_source Virtual source name produced by the algorithm.
 * @param[in] input_source Original source name copied from the snapshot.
 * @param[in] column_name Column name carried by the echoed frame.
 * @param[in] values Column sample container held by the batch variant.
 */
template <typename Values>
void logEchoColumnValues(
    const std::string& output_source,
    const std::string& input_source,
    const std::string& column_name,
    const Values&      values)
{
    using ValueType = typename Values::value_type;

    if constexpr (is_vector_v<ValueType>)
    {
        util::log::debugf(
            "EchoAlgorithm output_source={} input_source={} column={} array_samples={}",
            output_source,
            input_source,
            column_name,
            values.size());
    }
    else if constexpr (std::is_same_v<ValueType, bool>)
    {
        for (bool value : values)
        {
            util::log::debugf(
                "EchoAlgorithm output_source={} input_source={} column={} value={}",
                output_source,
                input_source,
                column_name,
                value ? "true" : "false");
        }
    }
    else
    {
        for (const auto& value : values)
        {
            util::log::debugf(
                "EchoAlgorithm output_source={} input_source={} column={} value={}",
                output_source,
                input_source,
                column_name,
                value);
        }
    }
}

} // namespace

void EchoAlgorithm::configure(const config::Config& cfg)
{
    output_source_ = cfg.get("output-source", "");
    if (!output_source_.empty())
    {
        return;
    }

    for (const auto& source_cfg : cfg.subConfig("sources"))
    {
        std::string source_name;
        source_cfg >> source_name;
        if (!source_name.empty())
        {
            output_source_ = source_name + "-echo";
            return;
        }
    }

    output_source_ = "VIRTUAL:ECHO:OUT";
}

std::vector<AlgorithmOutput> EchoAlgorithm::compute(const AlignedSnapshot& snapshot)
{
    if (snapshot.channels.empty())
    {
        return {};
    }

    const auto channel_it = snapshot.channels.begin();
    util::bus::TimeSeriesPayload payload;
    payload.root_source_name = output_source_;
    payload.frames.push_back(channel_it->second);

    const auto& frame = payload.frames.back();
    for (const auto& column : frame.columns)
    {
        std::visit(
            [&](const auto& values)
            {
                logEchoColumnValues(output_source_, channel_it->first, column.name, values);
            },
            column.values);
    }

    return {AlgorithmOutput{output_source_, std::move(payload)}};
}

REGISTER_ALGORITHM("echo", EchoAlgorithm);

} // namespace mldp_pvxs_driver::processor

#endif // BUILD_ECHO_PROCESSOR
