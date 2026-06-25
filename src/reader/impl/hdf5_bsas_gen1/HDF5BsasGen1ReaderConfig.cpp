//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/hdf5_bsas_gen1/HDF5BsasGen1ReaderConfig.h>

#include <config/ProvenanceConfig.h>

#include <map>

using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader::impl::hdf5_bsas_gen1;

HDF5BsasGen1ReaderConfig::HDF5BsasGen1ReaderConfig(const Config& readerEntry)
{
    if (!readerEntry.valid())
        throw Error("Reader entry is invalid");
    parse(readerEntry);
}

void HDF5BsasGen1ReaderConfig::parse(const Config& readerEntry)
{
    if (!readerEntry.hasChild("name"))
        throw Error(makeMissingFieldMessage("name"));
    name_ = readerEntry.get("name");
    if (name_.empty())
        throw Error("name must not be empty");

    if (!readerEntry.hasChild("file-path"))
        throw Error(makeMissingFieldMessage("file-path"));
    file_path_ = readerEntry.get("file-path");
    if (file_path_.empty())
        throw Error("file-path must not be empty");

    chunk_size_ = static_cast<std::size_t>(readerEntry.getInt("chunk-size", 1000L));
    if (chunk_size_ == 0)
        throw Error("chunk-size must be > 0");

    use_label_as_name_ = readerEntry.getBool("use-label-as-name", true);

    if (readerEntry.hasChild("metadata"))
    {
        std::map<std::string, std::string> m;
        readerEntry.subConfig("metadata").front() >> m;
        static_metadata_.insert(m.begin(), m.end());
    }

    provenance_ = config::parseProvenance(readerEntry);

    valid_ = true;
}
