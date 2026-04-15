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

std::unordered_map<std::string, WriterFactory::CreatorFn>&
WriterFactory::registry()
{
    static std::unordered_map<std::string, CreatorFn> instance;
    return instance;
}

void WriterFactory::registerType(const std::string& type, CreatorFn fn)
{
    registry()[type] = std::move(fn);
}

std::vector<std::string> WriterFactory::registeredTypes()
{
    auto&                    reg = registry();
    std::vector<std::string> types;
    types.reserve(reg.size());
    for (const auto& [type, fn] : reg)
    {
        types.push_back(type);
    }
    return types;
}

IWriterUPtr WriterFactory::create(
    const std::string&                type,
    const config::Config&             writerTypeNode,
    std::shared_ptr<metrics::Metrics> metrics)
{
    auto& reg = registry();
    auto  it = reg.find(type);
    if (it == reg.end())
    {
        throw std::runtime_error("Unknown writer type: " + type);
    }
    return it->second(writerTypeNode, std::move(metrics));
}
