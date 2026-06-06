//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/impl/LinearTransformAlgorithm.h>

#include <processor/ChannelProcessorFactory.h>

#include <stdexcept>
#include <utility>

namespace mldp_pvxs_driver::processor {

namespace {

double firstScalarValue(const util::bus::DataBatch& batch)
{
    if (batch.columns.empty())
    {
        return 0.0;
    }

    const auto* values = std::get_if<std::vector<double>>(&batch.columns.front().values);
    if (values == nullptr || values->empty())
    {
        return 0.0;
    }

    return values->front();
}

} // namespace

void LinearTransformAlgorithm::configure(const config::Config& cfg)
{
    output_source_ = cfg.get("output-source", "");
    if (output_source_.empty())
    {
        throw std::runtime_error("linear-transform requires a non-empty 'output-source'");
    }

    sources_.clear();
    for (const auto& source_cfg : cfg.subConfig("sources"))
    {
        std::string source_name;
        source_cfg >> source_name;
        if (!source_name.empty())
        {
            sources_.push_back(std::move(source_name));
        }
    }

    coefficients_.clear();
    for (const auto& coefficient_cfg : cfg.subConfig("coefficients"))
    {
        double coefficient = 0.0;
        coefficient_cfg >> coefficient;
        coefficients_.push_back(coefficient);
    }

    if (coefficients_.empty())
    {
        throw std::runtime_error("linear-transform requires a non-empty 'coefficients' list");
    }

    bias_ = cfg.getDouble("bias", 0.0);
    output_column_ = cfg.get("output-column", "result");
    if (output_column_.empty())
    {
        output_column_ = "result";
    }
}

std::vector<std::string> LinearTransformAlgorithm::outputSources() const noexcept
{
    return {output_source_};
}

std::vector<AlgorithmOutput> LinearTransformAlgorithm::compute(const AlignedSnapshot& snapshot)
{
    if (sources_.size() != coefficients_.size())
    {
        throw std::runtime_error("linear-transform requires one coefficient per configured source");
    }

    double result = bias_;
    for (std::size_t index = 0; index < sources_.size(); ++index)
    {
        const auto channel_it = snapshot.channels.find(sources_[index]);
        if (channel_it == snapshot.channels.end())
        {
            continue;
        }

        result += coefficients_[index] * firstScalarValue(channel_it->second);
    }

    util::bus::DataBatch frame;
    frame.timestamps.push_back({snapshot.reference_time.epoch_seconds, snapshot.reference_time.nanoseconds});
    frame.columns.push_back(util::bus::DataColumn{output_column_, std::vector<double>{result}});

    util::bus::TimeSeriesPayload payload;
    payload.root_source_name = output_source_;
    payload.frames.push_back(std::move(frame));
    payload.is_tabular = true;
    payload.end_of_batch_group = true;

    return {AlgorithmOutput{output_source_, std::move(payload)}};
}

std::string LinearTransformAlgorithm::algorithmType() const noexcept
{
    return "linear-transform";
}

REGISTER_ALGORITHM("linear-transform", LinearTransformAlgorithm);

} // namespace mldp_pvxs_driver::processor
