//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics/pvxs/EpicsPVXSBsasGen2Reader.h>

#include <algorithm>
#include <cctype>

using namespace mldp_pvxs_driver::reader::impl::epics;

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char x, unsigned char y)
                      {
                          return std::tolower(x) == std::tolower(y);
                      });
}

} // namespace

EpicsPVXSBsasGen2Reader::EpicsPVXSBsasGen2Reader(std::shared_ptr<util::bus::IDataBus> bus,
                                                 std::shared_ptr<metrics::Metrics>    metrics,
                                                 const config::Config&                cfg)
    : EpicsPVXSBsasTableReaderBase(std::move(bus), std::move(metrics), cfg,
                                   EpicsPVXSBsasTableReaderConfig::Generation::Gen2, kReaderType,
                                   &EpicsPVXSBsasGen2Reader::makeColumnFilter)
{
    addPV(pvNames());
}

std::optional<std::string> EpicsPVXSBsasGen2Reader::valueColumnName(std::string_view label, const TableOptions& opts)
{
    if (label == opts.pidField || label == opts.tsSecondsField || label == opts.tsNanosField)
    {
        return std::nullopt;
    }

    auto sep = label.rfind('.');
    if (sep == std::string_view::npos)
    {
        sep = label.rfind('_');
    }
    if (sep == std::string_view::npos || sep == 0)
    {
        return std::nullopt;
    }

    if (!equalsIgnoreCase(label.substr(sep + 1), opts.valueSuffix))
    {
        return std::nullopt;
    }
    return std::string(label.substr(0, sep));
}

EpicsPVXSBsasGen2Reader::ColumnFilter EpicsPVXSBsasGen2Reader::makeColumnFilter(const TableOptions& opts)
{
    return [opts](std::string_view label)
    {
        return valueColumnName(label, opts);
    };
}
