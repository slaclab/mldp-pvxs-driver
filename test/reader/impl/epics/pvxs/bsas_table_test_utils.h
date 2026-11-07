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
 * @file bsas_table_test_utils.h
 * @brief Shared helpers for the epics-pvxs-bsas-gen1 / -gen2 reader tests.
 */

#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <util/bus/IDataBus.h>

namespace bsas_table_test_utils {

using mldp_pvxs_driver::util::bus::DataBatch;
using mldp_pvxs_driver::util::bus::DataColumn;
using mldp_pvxs_driver::util::bus::IDataBus;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;
using EventBatch = IDataBus::EventBatch;

/// Bus that records every accepted batch; can be switched to reject pushes.
class RecordingBus : public IDataBus
{
public:
    bool push(EventBatch batch) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++push_calls_;
        if (reject_.load())
        {
            ++rejected_;
            cv_.notify_all();
            return false;
        }
        events_.push_back(std::move(batch));
        cv_.notify_all();
        return true;
    }

    void setReject(bool reject)
    {
        reject_ = reject;
    }

    std::size_t rejected() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }

    std::size_t pushCalls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return push_calls_;
    }

    std::vector<EventBatch> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    /// Wait until at least @p count end_of_batch_group markers for @p pv were accepted.
    bool waitForMarkers(const std::string& pv, std::size_t count,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]
                            {
                                return countMarkersLocked(pv) >= count;
                            });
    }

    /// Wait until at least @p count pushes were rejected.
    bool waitForRejected(std::size_t count,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&]
                            {
                                return rejected_ >= count;
                            });
    }

private:
    std::size_t countMarkersLocked(const std::string& pv) const
    {
        std::size_t n = 0;
        for (const auto& e : events_)
        {
            const auto* ts = std::get_if<TimeSeriesPayload>(&e.payload);
            if (ts && ts->end_of_batch_group && ts->root_source_name == pv)
            {
                ++n;
            }
        }
        return n;
    }

    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::vector<EventBatch>         events_;
    std::atomic<bool>               reject_{false};
    std::size_t                     rejected_{0};
    std::size_t                     push_calls_{0};
};

/// All bus batches produced by one NTTable update: data batches then marker.
struct Group
{
    std::vector<EventBatch> data;
    EventBatch              marker;

    /// All frames of the group, in push order.
    std::vector<const DataBatch*> frames() const
    {
        std::vector<const DataBatch*> out;
        for (const auto& b : data)
        {
            for (const auto& f : std::get<TimeSeriesPayload>(b.payload).frames)
            {
                out.push_back(&f);
            }
        }
        return out;
    }

    /// Names of every column of the group, in push order.
    std::vector<std::string> columnNames() const
    {
        std::vector<std::string> out;
        for (const auto* f : frames())
        {
            for (const auto& c : f->columns)
            {
                out.push_back(c.name);
            }
        }
        return out;
    }

    /// Every (DataBatch, column) chunk named @p name, in push order.
    std::vector<std::pair<const DataBatch*, const DataColumn*>> chunks(const std::string& name) const
    {
        std::vector<std::pair<const DataBatch*, const DataColumn*>> out;
        for (const auto* f : frames())
        {
            for (const auto& c : f->columns)
            {
                if (c.name == name)
                {
                    out.emplace_back(f, &c);
                }
            }
        }
        return out;
    }

    /// Find column @p name; sets @p frameOut to the frame holding it.
    const DataColumn* column(const std::string& name, const DataBatch** frameOut = nullptr) const
    {
        for (const auto* f : frames())
        {
            for (const auto& c : f->columns)
            {
                if (c.name == name)
                {
                    if (frameOut)
                    {
                        *frameOut = f;
                    }
                    return &c;
                }
            }
        }
        return nullptr;
    }
};

/// Split the batches for @p pv into complete groups (each ending with a marker).
inline std::vector<Group> groupsFor(const std::vector<EventBatch>& events, const std::string& pv)
{
    std::vector<Group> groups;
    Group              current;
    for (const auto& e : events)
    {
        const auto* ts = std::get_if<TimeSeriesPayload>(&e.payload);
        if (!ts || ts->root_source_name != pv)
        {
            continue;
        }
        if (ts->end_of_batch_group)
        {
            current.marker = e;
            groups.push_back(std::move(current));
            current = Group{};
        }
        else
        {
            current.data.push_back(e);
        }
    }
    return groups;
}

/// Complete groups for @p pv that carry at least one data batch.
inline std::vector<Group> dataGroupsFor(const std::vector<EventBatch>& events, const std::string& pv)
{
    std::vector<Group> out;
    for (auto& g : groupsFor(events, pv))
    {
        if (!g.data.empty())
        {
            out.push_back(std::move(g));
        }
    }
    return out;
}

/// Element-wise equality of two timestamp vectors.
inline bool sameTimestamps(const DataBatch& a, const DataBatch& b)
{
    if (a.timestamps.size() != b.timestamps.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.timestamps.size(); ++i)
    {
        if (a.timestamps[i].epoch_seconds != b.timestamps[i].epoch_seconds ||
            a.timestamps[i].nanoseconds != b.timestamps[i].nanoseconds)
        {
            return false;
        }
    }
    return true;
}

/// Return the values of @p col as doubles (any numeric variant alternative).
inline std::vector<double> asDoubles(const DataColumn& col)
{
    std::vector<double> out;
    std::visit(
        [&](const auto& vals)
        {
            using T = typename std::decay_t<decltype(vals)>::value_type;
            if constexpr (std::is_arithmetic_v<T>)
            {
                for (const auto& v : vals)
                {
                    out.push_back(static_cast<double>(v));
                }
            }
        },
        col.values);
    return out;
}

} // namespace bsas_table_test_utils
