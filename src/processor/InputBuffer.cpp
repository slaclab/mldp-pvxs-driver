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

#include <cstdint>

namespace mldp_pvxs_driver::processor {

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
                         AlignmentPolicy                 policy)
    : required_source_lookup_(source_names.begin(), source_names.end())
    , required_sources_(source_names)
    , alignment_(policy)
{
}

void InputBuffer::ingest(const std::string&                 root_source_name,
                         const util::bus::TimeSeriesPayload& payload)
{
    if (required_source_lookup_.find(root_source_name) == required_source_lookup_.end())
    {
        return;
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
        // Step 03 defers interpolation; for now every supported policy keeps
        // the latest full frame per source.
        slots_[root_source_name] = payload.frames.back();
        break;
    }

    fresh_.insert(root_source_name);
}

std::optional<AlignedSnapshot> InputBuffer::trySnapshot(TriggerPolicy trigger)
{
    if (trigger == TriggerPolicy::AllUpdated && fresh_.size() != required_sources_.size())
    {
        return std::nullopt;
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
}

} // namespace mldp_pvxs_driver::processor
