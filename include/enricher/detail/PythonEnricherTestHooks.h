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

#if defined(BUILD_PYTHON_PROCESSOR) && defined(MLDP_PYTHON_ENRICHER_TEST_HOOKS)

#include <cstddef>

namespace mldp_pvxs_driver::enricher::detail {

enum class PythonConversionFailure
{
    batchDictionary,
    metadataString
};

void failNextPythonConversion(PythonConversionFailure failure);
std::size_t metadataDictionaryCleanupCount();

} // namespace mldp_pvxs_driver::enricher::detail

#endif // defined(BUILD_PYTHON_PROCESSOR) && defined(MLDP_PYTHON_ENRICHER_TEST_HOOKS)
