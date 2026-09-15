//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/OstreamOutputStream.h>

using namespace mldp_pvxs_driver::query;

OstreamOutputStream::OstreamOutputStream(std::ostream& output)
    : output_(output)
{
}

arrow::Status OstreamOutputStream::Close()
{
    closed_ = true;
    output_.flush();
    return output_ ? arrow::Status::OK() : arrow::Status::IOError("Failed to flush Arrow IPC output");
}

bool OstreamOutputStream::closed() const
{
    return closed_;
}

arrow::Result<int64_t> OstreamOutputStream::Tell() const
{
    return position_;
}

arrow::Status OstreamOutputStream::Write(const void* data, const int64_t nbytes)
{
    if (closed_) return arrow::Status::Invalid("Cannot write to a closed Arrow IPC output stream");
    output_.write(static_cast<const char*>(data), nbytes);
    if (!output_) return arrow::Status::IOError("Failed to write Arrow IPC output");
    position_ += nbytes;
    return arrow::Status::OK();
}
