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
    /** @brief Constructs an output stream adapter wrapping the caller-owned ostream.
     * @param[in,out] output  Destination ostream; must outlive this adapter. */
    explicit OstreamOutputStream(std::ostream& output);

    /** @brief Closes the stream; subsequent writes will fail.
     * @return Arrow Status::OK(). */
    arrow::Status Close() override;

    /** @brief Returns true if Close() has been called.
     * @return True when closed. */
    bool closed() const override;

    /** @brief Returns the current write position in bytes.
     * @return Arrow Result with the byte offset. */
    arrow::Result<int64_t> Tell() const override;

    /** @brief Writes nbytes from data to the wrapped ostream.
     * @param[in] data    Source buffer.
     * @param[in] nbytes  Number of bytes to write.
     * @return Arrow Status. */
    arrow::Status Write(const void* data, int64_t nbytes) override;

private:
    std::ostream& output_;       ///< Destination ostream; not owned.
    int64_t       position_{0};  ///< Current write position in bytes.
    bool          closed_{false}; ///< True after Close() is called.
};

} // namespace mldp_pvxs_driver::query
