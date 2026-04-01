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

#include "MockArchiverPbHttpServer.h"
#include "MockDataBus.h"

#include <chrono>
#include <thread>

namespace mldp_pvxs_driver::test::mock {

/// Waits for the mock archiver HTTP server to receive a request and complete its response.
///
/// Polls until the server's last request path is non-empty (indicating a request was received),
/// then waits for that response to complete.
///
/// @param server Reference to the MockArchiverPbHttpServer instance
/// @param timeout Maximum time to wait in milliseconds
/// @return true if request was received and response completed within timeout, false otherwise
inline bool waitForMockRequestStartAndCompletion(
    const mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer& server,
    std::chrono::milliseconds                                                       timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!server.lastRequest().path.empty())
        {
            return server.waitForLastResponseComplete(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// Waits for the mock archiver HTTP server to receive at least one request.
///
/// Polls until the server's last request path is non-empty, indicating that a request
/// has been received by the mock server.
///
/// @param server Reference to the MockArchiverPbHttpServer instance
/// @param timeout Maximum time to wait in milliseconds
/// @return true if a request was received within timeout, false otherwise
inline bool waitForMockRequestStart(
    const mldp_pvxs_driver::reader::impl::epics_archiver::MockArchiverPbHttpServer& server,
    std::chrono::milliseconds                                                       timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!server.lastRequest().path.empty())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// Waits for the event bus to publish at least a minimum number of event batches.
///
/// Polls the mock event bus and checks if the snapshot contains at least the specified
/// number of batches. Useful for verifying that a reader has published expected data.
///
/// @param bus Reference to the MockDataBus data bus instance
/// @param min_batches Minimum number of batches expected to be published
/// @param timeout Maximum time to wait in milliseconds
/// @return true if at least min_batches were published within timeout, false otherwise
inline bool waitForAtLeastPublishedBatches(const MockDataBus& bus, size_t min_batches, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (bus.snapshot().size() >= min_batches)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return bus.snapshot().size() >= min_batches;
}

} // namespace mldp_pvxs_driver::test::mock
