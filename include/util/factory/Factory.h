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

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mldp_pvxs_driver::util::factory {

/**
 * @brief CRTP base providing a static type-keyed factory registry.
 *
 * Each concrete factory class inherits from Factory<Derived, ProductT, CtorArgs...>
 * and must declare:
 * @code
 *   static constexpr std::string_view kTypeName = "myproduct";
 * @endcode
 *
 * That label is used in error messages when an unknown type key is requested.
 *
 * @tparam Derived    The concrete factory class (CRTP self-type).
 * @tparam ProductT   The abstract base type produced by this factory.
 * @tparam CtorArgs   Argument types forwarded to the concrete type's constructor.
 */
template <typename Derived, typename ProductT, typename... CtorArgs>
class Factory {
public:
    /// Owning pointer to a newly created product instance.
    using UPtr = std::unique_ptr<ProductT>;

    /// Callable that constructs a product from CtorArgs.
    using CreatorFn = std::function<UPtr(CtorArgs...)>;

    // Non-instantiable — all members are static.
    Factory() = delete;

    /**
     * @brief Register a named creator function.
     *
     * Calling this with an already-registered @p name silently overwrites the
     * previous creator.
     *
     * @param name  Unique string key that identifies the concrete type.
     * @param fn    Factory callable; must return a non-null UPtr.
     */
    static void registerType(const std::string& name, CreatorFn fn) {
        registry()[name] = std::move(fn);
    }

    /**
     * @brief Construct and return a product by its registered name.
     *
     * @param name  Key previously passed to registerType().
     * @param args  Constructor arguments forwarded to the creator function.
     * @return      Owning pointer to the newly created product.
     * @throws std::runtime_error if @p name is not registered.
     */
    static UPtr create(const std::string& name, CtorArgs... args) {
        auto& reg = registry();
        auto it   = reg.find(name);
        if (it == reg.end()) {
            throw std::runtime_error(
                "Unknown " + std::string(Derived::kTypeName) + " type: " + name);
        }
        return it->second(std::forward<CtorArgs>(args)...);
    }

    /**
     * @brief Return the names of all currently registered types.
     *
     * @return Vector of registered type name strings (order unspecified).
     */
    static std::vector<std::string> registeredTypes() {
        auto& reg = registry();
        std::vector<std::string> types;
        types.reserve(reg.size());
        for (const auto& [k, v] : reg) {
            types.push_back(k);
        }
        return types;
    }

private:
    /// Meyers-singleton registry shared by all calls within a given Derived+ProductT
    /// instantiation.
    static std::unordered_map<std::string, CreatorFn>& registry() {
        static std::unordered_map<std::string, CreatorFn> instance;
        return instance;
    }
};

/**
 * @brief Registration helper that auto-registers a concrete type at static-init time.
 *
 * Declare one static instance per concrete type, typically inside an anonymous
 * namespace in the concrete type's translation unit:
 * @code
 *   namespace {
 *     const mldp_pvxs_driver::util::factory::FactoryRegistrator<
 *         ReaderFactory, EpicsArchiverReader>
 *         sReg{"epics_archiver"};
 *   }
 * @endcode
 *
 * @tparam FactoryT   The factory class (must expose registerType / UPtr).
 * @tparam ConcreteT  The concrete product type to register.
 */
template <typename FactoryT, typename ConcreteT>
class FactoryRegistrator {
public:
    /**
     * @brief Registers ConcreteT under @p name in FactoryT's registry.
     *
     * @param name  The key under which ConcreteT will be retrievable via
     *              FactoryT::create().
     */
    explicit FactoryRegistrator(const char* name) {
        FactoryT::registerType(
            name,
            [](auto&&... args) -> typename FactoryT::UPtr {
                return std::make_unique<ConcreteT>(
                    std::forward<decltype(args)>(args)...);
            });
    }
};

}  // namespace mldp_pvxs_driver::util::factory
