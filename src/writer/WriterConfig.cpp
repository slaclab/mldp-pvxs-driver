//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/WriterConfig.h>
#include <writer/WriterFactory.h>
#include <writer/mldp/MLDPWriterConfig.h>
#include <writer/mldp_pv_metadata/MLDPPVMetadataWriterConfig.h>
#include <writer/mldp_configuration/MLDPConfigurationWriterConfig.h>

#ifdef MLDP_PVXS_HDF5_ENABLED
    #include <writer/hdf5/HDF5WriterConfig.h>
#endif

#include <functional>
#include <unordered_map>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::config;

namespace {

// Per-type deep validators for types that need it. Types absent from this map
// (e.g. test writers registered at runtime) skip deep validation.
using ValidatorFn = std::function<void(const Config&)>;

const std::unordered_map<std::string, ValidatorFn>& deepValidators()
{
    static const std::unordered_map<std::string, ValidatorFn> kValidators = {
        {"mldp",
         [](const Config& item) {
             try { MLDPWriterConfig::parse(item); }
             catch (const MLDPWriterConfig::Error& e) { throw WriterConfig::Error(std::string("writer.mldp: ") + e.what()); }
         }},
        {"mldp-pv-metadata",
         [](const Config& item) {
             try { MLDPPVMetadataWriterConfig::parse(item); }
             catch (const std::runtime_error& e) { throw WriterConfig::Error(std::string("writer.mldp-pv-metadata: ") + e.what()); }
         }},
        {"mldp-configuration",
         [](const Config& item) {
             try { MLDPConfigurationWriterConfig::parse(item); }
             catch (const std::runtime_error& e) { throw WriterConfig::Error(std::string("writer.mldp-configuration: ") + e.what()); }
         }},
#ifdef MLDP_PVXS_HDF5_ENABLED
        {"hdf5",
         [](const Config& item) {
             try { HDF5WriterConfig::parse(item); }
             catch (const HDF5WriterConfig::Error& e) { throw WriterConfig::Error(std::string("writer.hdf5: ") + e.what()); }
         }},
        {"hdf5-merge",
         [](const Config& item) {
             try { HDF5WriterConfig::parse(item); }
             catch (const HDF5WriterConfig::Error& e) { throw WriterConfig::Error(std::string("writer.hdf5-merge: ") + e.what()); }
         }},
#else
        {"hdf5",
         [](const Config&) { throw WriterConfig::Error("writer.hdf5 configured but HDF5 support not compiled in (MLDP_PVXS_ENABLE_HDF5=OFF)"); }},
        {"hdf5-merge",
         [](const Config&) { throw WriterConfig::Error("writer.hdf5-merge configured but HDF5 support not compiled in (MLDP_PVXS_HDF5_ENABLED=OFF)"); }},
#endif
    };
    return kValidators;
}

} // namespace

void WriterConfig::validate(const Config& writerNode)
{
    int instanceCount = 0;

    for (const auto& typeName : WriterFactory::registeredTypes())
    {
        if (!writerNode.hasChild(typeName))
            continue;

        if (!writerNode.isSequence(typeName))
        {
            throw Error("writer." + typeName + " must be a sequence of writer instances");
        }

        const auto items = writerNode.subConfig(typeName);
        const auto& validators = deepValidators();
        auto it = validators.find(typeName);
        if (it != validators.end())
        {
            for (const auto& item : items)
                it->second(item);
        }

        instanceCount += static_cast<int>(items.size());
    }

    if (instanceCount == 0)
    {
        throw std::invalid_argument("writer config: at least one writer instance must be configured");
    }
}
