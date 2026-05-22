//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/mldp_pv_metadata/MLDPPVMetadataWriterConfig.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::pool;

MLDPPVMetadataWriterConfig MLDPPVMetadataWriterConfig::parse(const config::Config& node)
{
    MLDPPVMetadataWriterConfig cfg;

    cfg.name = node.get("name", "");
    if (cfg.name.empty())
    {
        throw std::runtime_error("MLDPPVMetadataWriterConfig: 'name' is required");
    }

    cfg.threadPool = node.getInt("thread-pool", 2);
    cfg.deadlineSeconds = node.getInt("deadline-seconds", 10);

    if (!node.hasChild("mldp-pv-metadata-pool"))
    {
        throw std::runtime_error(
            "MLDPPVMetadataWriterConfig: 'mldp-pv-metadata-pool' block is required");
    }
    const auto poolNodes = node.subConfig("mldp-pv-metadata-pool");
    if (poolNodes.empty())
    {
        throw std::runtime_error(
            "MLDPPVMetadataWriterConfig: 'mldp-pv-metadata-pool' block is empty");
    }
    cfg.poolConfig = MLDPGrpcPoolConfig(poolNodes.front());

    return cfg;
}
