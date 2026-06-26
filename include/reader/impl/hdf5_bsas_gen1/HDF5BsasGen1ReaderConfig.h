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

#include <config/Config.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1 {

class HDF5BsasGen1ReaderConfig
{
public:
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    HDF5BsasGen1ReaderConfig() = default;
    explicit HDF5BsasGen1ReaderConfig(const config::Config& readerEntry);

    bool valid() const { return valid_; }
    const std::string& name() const { return name_; }
    const std::string& filePath() const { return file_path_; }
    std::size_t chunkSize() const { return chunk_size_; }
    const std::unordered_map<std::string, std::string>& staticMetadata() const { return static_metadata_; }
    const std::unordered_map<std::string, std::string>& provenance() const { return provenance_; }
    std::size_t logIntervalSec() const { return log_interval_sec_; }

private:
    void parse(const config::Config& readerEntry);

    bool valid_ = false;
    std::string name_;
    std::string file_path_;
    std::size_t chunk_size_ = 1000;
    std::size_t log_interval_sec_ = 10;
    std::unordered_map<std::string, std::string> static_metadata_;
    std::unordered_map<std::string, std::string> provenance_;
};

} // namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1
