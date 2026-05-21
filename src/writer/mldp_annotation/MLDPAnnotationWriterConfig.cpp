//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/mldp_annotation/MLDPAnnotationWriterConfig.h>

#include <stdexcept>

using namespace mldp_pvxs_driver::writer;
using namespace mldp_pvxs_driver::util::pool;

MLDPAnnotationWriterConfig MLDPAnnotationWriterConfig::parse(const config::Config& node)
{
    MLDPAnnotationWriterConfig cfg;

    cfg.name = node.get("name", "");
    if (cfg.name.empty())
    {
        throw std::runtime_error("MLDPAnnotationWriterConfig: 'name' is required");
    }

    cfg.threadPool = node.getInt("thread-pool", 2);
    cfg.deadlineSeconds = node.getInt("deadline-seconds", 10);

    if (!node.hasChild("mldp-annotation-pool"))
    {
        throw std::runtime_error(
            "MLDPAnnotationWriterConfig: 'mldp-annotation-pool' block is required");
    }
    const auto poolNodes = node.subConfig("mldp-annotation-pool");
    if (poolNodes.empty())
    {
        throw std::runtime_error(
            "MLDPAnnotationWriterConfig: 'mldp-annotation-pool' block is empty");
    }
    cfg.poolConfig = MLDPGrpcPoolConfig(poolNodes.front());

    return cfg;
}
