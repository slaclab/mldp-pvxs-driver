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
#include <writer/mldp/MLDPWriterConfig.h>
#include <writer/mldp_annotation/MLDPAnnotationWriterConfig.h>
#include <writer/mldp_configuration/MLDPConfigurationWriterConfig.h>

#ifdef MLDP_PVXS_HDF5_ENABLED
#include <writer/hdf5/HDF5WriterConfig.h>
#endif

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::config;

void WriterConfig::validate(const Config& writerNode)
{
    int instanceCount = 0;

    // -- MLDP writer instances — parse each to catch per-instance errors --
    if (writerNode.hasChild(WriterMldpKey))
    {
        if (!writerNode.isSequence(WriterMldpKey))
        {
            throw Error("writer.mldp must be a sequence of writer instances");
        }
        const auto items = writerNode.subConfig(WriterMldpKey);
        for (const auto& item : items)
        {
            try
            {
                MLDPWriterConfig::parse(item);
            }
            catch (const MLDPWriterConfig::Error& e)
            {
                throw Error(std::string("writer.mldp: ") + e.what());
            }
        }
        instanceCount += static_cast<int>(items.size());
    }

    // -- HDF5 writer instances --
    if (writerNode.hasChild(WriterHdf5Key))
    {
        if (!writerNode.isSequence(WriterHdf5Key))
        {
            throw Error("writer.hdf5 must be a sequence of writer instances");
        }
#ifndef MLDP_PVXS_HDF5_ENABLED
        throw Error("writer.hdf5 instances are configured but this build was compiled without "
                    "HDF5 support (MLDP_PVXS_ENABLE_HDF5=OFF)");
#else
        const auto items = writerNode.subConfig(WriterHdf5Key);
        for (const auto& item : items)
        {
            try
            {
                HDF5WriterConfig::parse(item);
            }
            catch (const HDF5WriterConfig::Error& e)
            {
                throw Error(std::string("writer.hdf5: ") + e.what());
            }
        }
        instanceCount += static_cast<int>(items.size());
#endif
    }

    // -- HDF5 merge writer instances --
    if (writerNode.hasChild(WriterHdf5MergeKey))
    {
        if (!writerNode.isSequence(WriterHdf5MergeKey))
        {
            throw Error("writer.hdf5-merge must be a sequence of writer instances");
        }
#ifndef MLDP_PVXS_HDF5_ENABLED
        throw Error("writer.hdf5-merge instances are configured but this build was compiled without "
                    "HDF5 support (MLDP_PVXS_HDF5_ENABLED=OFF)");
#else
        const auto mergeItems = writerNode.subConfig(WriterHdf5MergeKey);
        for (const auto& item : mergeItems)
        {
            try
            {
                HDF5WriterConfig::parse(item);
            }
            catch (const HDF5WriterConfig::Error& e)
            {
                throw Error(std::string("writer.hdf5-merge: ") + e.what());
            }
        }
        instanceCount += static_cast<int>(mergeItems.size());
#endif
    }

    // -- MLDP annotation writer instances --
    if (writerNode.hasChild(WriterMldpAnnotationKey))
    {
        if (!writerNode.isSequence(WriterMldpAnnotationKey))
        {
            throw Error("writer.mldp-annotation must be a sequence of writer instances");
        }
        const auto items = writerNode.subConfig(WriterMldpAnnotationKey);
        for (const auto& item : items)
        {
            try
            {
                MLDPAnnotationWriterConfig::parse(item);
            }
            catch (const std::runtime_error& e)
            {
                throw Error(std::string("writer.mldp-annotation: ") + e.what());
            }
        }
        instanceCount += static_cast<int>(items.size());
    }

    // -- MLDP configuration writer instances --
    if (writerNode.hasChild(WriterMldpConfigurationKey))
    {
        if (!writerNode.isSequence(WriterMldpConfigurationKey))
        {
            throw Error("writer.mldp-configuration must be a sequence of writer instances");
        }
        const auto items = writerNode.subConfig(WriterMldpConfigurationKey);
        for (const auto& item : items)
        {
            try
            {
                MLDPConfigurationWriterConfig::parse(item);
            }
            catch (const std::runtime_error& e)
            {
                throw Error(std::string("writer.mldp-configuration: ") + e.what());
            }
        }
        instanceCount += static_cast<int>(items.size());
    }

    if (instanceCount == 0)
    {
        throw std::invalid_argument("writer config: at least one writer instance must be configured");
    }
}
