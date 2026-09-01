//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <writer/WriterFactory.h>

using namespace mldp_pvxs_driver::writer;

IWriterUPtr WriterFactory::create(
    const std::string&                type,
    const config::Config&             writerTypeNode,
    std::shared_ptr<metrics::Metrics> metrics)
{
    return Factory::create(type, writerTypeNode, std::move(metrics));
}
