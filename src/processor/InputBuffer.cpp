//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/InputBuffer.h>

#include <algorithm>
#include <cstdint>
#include <variant>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::processor;

namespace {

uint64_t timestampToNanoseconds(const util::bus::TimestampEntry& timestamp)
{
    return (timestamp.epoch_seconds * 1000000000ULL) + timestamp.nanoseconds;
}

util::bus::BusTimestamp maxReferenceTime(const std::unordered_map<std::string, util::bus::DataBatch>& slots)
{
    util::bus::BusTimestamp reference_time;
    uint64_t                best_value = 0;

    for (const auto& [source, batch] : slots)
    {
        (void)source;
        if (batch.timestamps.empty())
        {
            continue;
        }

        const auto& latest = batch.timestamps.back();
        const auto  value = timestampToNanoseconds(latest);
        if (value >= best_value)
        {
            best_value = value;
            reference_time.epoch_seconds = latest.epoch_seconds;
            reference_time.nanoseconds = latest.nanoseconds;
        }
    }

    return reference_time;
}

} // namespace

InputBuffer::InputBuffer(const std::vector<std::string>& source_names,
                         AlignmentPolicy                 policy,
                         std::size_t                     max_depth)
    : required_source_lookup_(source_names.begin(), source_names.end())
    , required_sources_(source_names)
    , alignment_(policy)
    , max_depth_(max_depth)
{
}

namespace {

void trimOldestSamples(util::bus::DataBatch& batch, std::size_t max_depth)
{
    if (max_depth == 0 || batch.timestamps.size() <= max_depth)
    {
        return;
    }

    const auto drop_count = batch.timestamps.size() - max_depth;

    // Pre-check: if any column is shorter than drop_count the batch is malformed.
    // Clear everything to keep timestamps/values shape consistent.
    for (const auto& column : batch.columns)
    {
        const bool underflow = std::visit(
            [drop_count](const auto& values)
            {
                return values.size() <= drop_count;
            },
            column.values);
        if (underflow)
        {
            batch.timestamps.clear();
            for (auto& col : batch.columns)
                std::visit([](auto& v)
                           {
                               v.clear();
                           },
                           col.values);
            return;
        }
    }

    batch.timestamps.erase(batch.timestamps.begin(), batch.timestamps.begin() + static_cast<std::ptrdiff_t>(drop_count));
    for (auto& column : batch.columns)
    {
        std::visit(
            [drop_count](auto& values)
            {
                values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(drop_count));
            },
            column.values);
    }
}

} // namespace

void InputBuffer::ingest(const std::string&                  root_source_name,
                         const util::bus::TimeSeriesPayload& payload)
{
    if (!required_source_lookup_.empty() &&
        required_source_lookup_.find(root_source_name) == required_source_lookup_.end())
    {
        return;
    }

    if (required_source_lookup_.empty())
    {
        seen_sources_.insert(root_source_name);
    }

    if (payload.frames.empty())
    {
        return;
    }

    switch (alignment_)
    {
    case AlignmentPolicy::LatestValue:
    case AlignmentPolicy::AllUpdated:
    case AlignmentPolicy::Interpolate:
        if (alignment_ == AlignmentPolicy::LatestValue)
        {
            slots_[root_source_name] = payload.frames.back();
            trimOldestSamples(slots_[root_source_name], max_depth_);
            break;
        }

        for (const auto& frame : payload.frames)
        {
            auto& slot = slots_[root_source_name];
            if (slot.columns.empty())
            {
                slot.columns = frame.columns;
            }
            else if (slot.columns.size() == frame.columns.size())
            {
                for (std::size_t i = 0; i < frame.columns.size(); ++i)
                {
                    std::visit(
                        [&](const auto& incoming_values)
                        {
                            using ValueVector = std::decay_t<decltype(incoming_values)>;
                            auto* existing_values = std::get_if<ValueVector>(&slot.columns[i].values);
                            if (existing_values == nullptr)
                            {
                                slot.columns[i].values = incoming_values;
                            }
                            else
                            {
                                existing_values->insert(existing_values->end(), incoming_values.begin(), incoming_values.end());
                            }
                        },
                        frame.columns[i].values);
                }
            }

            slot.timestamps.insert(slot.timestamps.end(), frame.timestamps.begin(), frame.timestamps.end());
            trimOldestSamples(slot, max_depth_);
        }
        break;
    }

    fresh_.insert(root_source_name);
}

std::optional<AlignedSnapshot> InputBuffer::trySnapshot(TriggerPolicy trigger)
{
    if (trigger == TriggerPolicy::AllUpdated)
    {
        if (!required_sources_.empty())
        {
            if (fresh_.size() != required_sources_.size())
                return std::nullopt;
        }
        else
        {
            if (fresh_.size() != seen_sources_.size())
                return std::nullopt;
        }
    }

    AlignedSnapshot snapshot;
    snapshot.channels = slots_;
    snapshot.reference_time = maxReferenceTime(slots_);
    return snapshot;
}

void InputBuffer::resetFreshFlags()
{
    fresh_.clear();
}

void InputBuffer::clear()
{
    slots_.clear();
    fresh_.clear();
    seen_sources_.clear();
}

std::size_t InputBuffer::bufferDepth() const noexcept
{
    std::size_t depth = 0;
    for (const auto& [source, batch] : slots_)
    {
        (void)source;
        depth = std::max(depth, batch.timestamps.size());
    }
    return depth;
}

std::size_t InputBuffer::bufferDepthForSource(const std::string& root_source_name) const noexcept
{
    const auto it = slots_.find(root_source_name);
    if (it == slots_.end())
    {
        return 0;
    }
    return it->second.timestamps.size();
}
