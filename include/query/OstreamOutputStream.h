//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file OstreamOutputStream.h
 * @brief Adapts a standard output stream to Arrow's OutputStream interface. */
#pragma once

#include <arrow/io/interfaces.h>
#include <arrow/result.h>

#include <ostream>

namespace mldp_pvxs_driver::query {

/** @brief Writes Arrow output directly to a caller-owned std::ostream. */
class OstreamOutputStream final : public arrow::io::OutputStream
{
public:
    explicit OstreamOutputStream(std::ostream& output);

    arrow::Status Close() override;
    bool closed() const override;
    arrow::Result<int64_t> Tell() const override;
    arrow::Status Write(const void* data, int64_t nbytes) override;

private:
    std::ostream& output_;
    int64_t position_{0};
    bool closed_{false};
};

} // namespace mldp_pvxs_driver::query
