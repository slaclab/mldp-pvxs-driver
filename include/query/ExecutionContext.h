//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <string>

namespace arrow {
class MemoryPool;
namespace fs {
class FileSystem;
}
} // namespace arrow

namespace mldp_pvxs_driver::query {

class SpillManager;

struct ExecutionContext {
    arrow::MemoryPool*                    pool{nullptr};
    std::shared_ptr<SpillManager>         spill;
    uint64_t                              memory_limit_bytes{0};
    uint32_t                              join_batch_size{0};
    std::shared_ptr<arrow::fs::FileSystem> spill_fs;
    std::string                           spill_dir;
};

} // namespace mldp_pvxs_driver::query
