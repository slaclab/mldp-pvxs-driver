//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <chrono>
#include <memory>

#include <arrow/compute/api.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/filesystem/mockfs.h>
#include <arrow/ipc/api.h>
#include <arrow/memory_pool.h>
#include <gtest/gtest.h>

namespace {

TEST(ArrowDependencyTest, CoreComputeFilesystemAndIpcAreAvailable)
{
    ASSERT_NE(arrow::default_memory_pool(), nullptr);

    [[maybe_unused]] arrow::fs::LocalFileSystem local_file_system;
    [[maybe_unused]] auto mock_file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    [[maybe_unused]] arrow::compute::FunctionOptions* compute_options = nullptr;
    [[maybe_unused]] arrow::ipc::IpcWriteOptions ipc_write_options = arrow::ipc::IpcWriteOptions::Defaults();
}

} // namespace
