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
#include <metrics/Metrics.h>
#include <util/factory/Factory.h>
#include <writer/IWriter.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::writer {

/**
 * @brief Central factory that tracks available writer implementations and builds them on demand.
 *
 * Writer implementations register themselves at static-initialisation time via
 * the @ref REGISTER_WRITER macro, which places a @ref WriterRegistrator into
 * the translation unit that defines each concrete writer.  The controller calls
 * @ref create for each configured writer type without needing to know anything
 * about the concrete classes.
 *
 * This mirrors the @c ReaderFactory pattern under `include/reader/`.
 */
class WriterFactory
    : public util::factory::Factory<WriterFactory, IWriter, const config::Config&, std::shared_ptr<metrics::Metrics>>
{
public:
    static constexpr std::string_view kTypeName = "writer";

    /**
     * @brief Construct a writer of @p type configured via @p writerTypeNode.
     *
     * @throws std::runtime_error if @p type has not been registered.
     */
    static IWriterUPtr create(
        const std::string&                type,
        const config::Config&             writerTypeNode,
        std::shared_ptr<metrics::Metrics> metrics = nullptr);
};

/**
 * @brief CRTP helper that registers a writer implementation during static initialisation.
 *
 * Each concrete writer declares one of these as a `static inline` member via
 * the @ref REGISTER_WRITER macro.  The constructor calls
 * @c WriterFactory::registerType with a lambda that forwards the two standard
 * factory arguments to the writer's `(config::Config, shared_ptr<Metrics>)` constructor.
 */
template <typename WriterT>
class WriterRegistrator
{
public:
    explicit WriterRegistrator(const char* typeName)
    {
        WriterFactory::registerType(
            typeName,
            [](const config::Config&             node,
               std::shared_ptr<metrics::Metrics> metrics)
            {
                return std::make_unique<WriterT>(node, std::move(metrics));
            });
    }
};

/**
 * @brief One-line macro that binds @c CLASSNAME into the factory under @c TYPE_STRING.
 *
 * Place this macro inside the class body of a concrete writer (but outside any
 * method).  The resulting `static inline` member is initialised before `main()`
 * and inserts the creator function into the factory registry.
 *
 * Example:
 * @code
 * class MLDPGrpcWriter final : public IWriter {
 *     REGISTER_WRITER("grpc", MLDPGrpcWriter)
 *     …
 * };
 * @endcode
 */
#define REGISTER_WRITER(TYPE_STRING, CLASSNAME) \
    static inline ::mldp_pvxs_driver::writer::WriterRegistrator<CLASSNAME> registrator_{TYPE_STRING};

} // namespace mldp_pvxs_driver::writer
