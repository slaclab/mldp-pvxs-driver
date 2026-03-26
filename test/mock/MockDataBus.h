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

#include <util/bus/IDataBus.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::test::mock {

/// Mock implementation of IDataBus that captures and stores event batches for testing.
///
/// This test double records all event batches pushed to it and provides thread-safe
/// access to a snapshot of received batches. It implements the full IDataBus interface
/// but only captures events via the `push()` method; other query methods return empty
/// results or null values as they are not used in integration tests.
///
/// Typical usage:
/// @code
///   auto bus = std::make_shared<MockDataBus>();
///   auto reader = std::make_unique<EpicsArchiverReader>(bus, nullptr, config);
///   // ... wait for processing ...
///   const auto batches = bus->snapshot();
///   EXPECT_EQ(batches.size(), expected_count);
/// @endcode
///
/// Thread safety: All methods are thread-safe. The `push()` and `snapshot()` methods
/// use a mutex to protect access to the underlying batch storage.
class MockDataBus final : public util::bus::IDataBus
{
public:
    using EventBatch = util::bus::IDataBus::EventBatch;
    using SourceInfo = util::bus::IDataBus::SourceInfo;

    /// Records an event batch received from a reader for later inspection.
    ///
    /// Thread-safe. The batch is moved into internal storage for retention.
    ///
    /// @param batch The event batch to record
    /// @return true (always accepts the batch)
    bool push(EventBatch batch) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        received_.emplace_back(std::move(batch));
        return true;
    }

    /// Returns empty source info list (not used in these tests).
    ///
    /// @param (unnamed) Set of source names to query (ignored)
    /// @return Empty vector
    std::vector<SourceInfo> querySourcesInfo(const std::set<std::string>&) override
    {
        return {};
    }

    /// Returns null optional for source data queries (not used in these tests).
    ///
    /// @param (unnamed) Set of source names to query (ignored)
    /// @param (unnamed) Query options (ignored)
    /// @return std::nullopt
    std::optional<std::unordered_map<std::string, std::vector<dp::service::common::DataValues>>> querySourcesData(
        const std::set<std::string>&,
        const util::bus::QuerySourcesDataOptions&) override
    {
        return std::nullopt;
    }

    /// Returns a thread-safe snapshot of all batches received so far.
    ///
    /// This is the primary method used by tests to verify that a reader has
    /// published the expected number and content of event batches.
    ///
    /// @return Vector copy of all recorded batches at call time
    std::vector<EventBatch> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return received_;
    }

private:
    /// Mutex protecting access to received_ batch storage
    mutable std::mutex mu_;
    
    /// Vector storing all batches received via push() calls
    std::vector<EventBatch> received_;
};

} // namespace mldp_pvxs_driver::test::mock
