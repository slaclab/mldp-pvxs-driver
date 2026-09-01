//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/** @file QueryableFactory.h
 * @brief Creates configured queryable implementations by registered table. */
#pragma once
#include <config/Config.h>
#include <metrics/Metrics.h>
#include <query/IQueryable.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <set>

namespace mldp_pvxs_driver::query {

/** @brief Thread-safe registry of configured queryable implementation factories. */
class QueryableFactory
{
public:
    /** @brief Returns the process-wide singleton factory instance.
     *  @return Reference to the singleton. */
    static QueryableFactory& instance()
    {
        static QueryableFactory inst;
        return inst;
    }

    /** @brief Registers queryable type T for all tables declared in T::kVirtualTables.
     *  @details Must be called before any create<T>() or createByTable() call for those tables.
     *  @tparam T Queryable implementation type; must expose a static kVirtualTables range and
     *            accept (Config, Metrics) construction.
     *  @param[in] cfg     Configuration passed to every T instance created by this factory.
     *  @param[in] metrics Optional metrics sink.
     *  @throws std::runtime_error If any virtual table is already registered. */
    template <typename T>
    void prepare(const config::Config&             cfg,
                 std::shared_ptr<metrics::Metrics> metrics = nullptr)
    {
        const auto creator = [cfg, metrics]() -> IQueryableUPtr
        {
            return std::make_unique<T>(cfg, metrics);
        };
        std::unique_lock lock(mutex_);
        for (const auto table : T::kVirtualTables)
        {
            const std::string table_name{table};
            auto it = table_creators_.find(table_name);
            if (it != table_creators_.end())
            {
                throw std::runtime_error("QueryableFactory: duplicate virtual table registration: " + table_name);
            }
        }
        creators_[std::type_index(typeid(T))] = creator;
        for (const auto table : T::kVirtualTables)
        {
            table_creators_.emplace(std::string(table), TableCreator{creator, std::type_index(typeid(T))});
        }
    }

    /** @brief Creates a new instance of T using the configuration registered via prepare<T>().
     *  @tparam T Queryable implementation type.
     *  @return Unique pointer to the new T instance.
     *  @throws std::runtime_error If T was not previously registered via prepare<T>(). */
    template <typename T>
    std::unique_ptr<T> create()
    {
        std::function<IQueryableUPtr()> creator;
        {
            std::shared_lock lock(mutex_);
            auto             it = creators_.find(std::type_index(typeid(T)));
            if (it == creators_.end())
                throw std::runtime_error(
                    std::string("QueryableFactory: type not prepared: ") + typeid(T).name());
            creator = it->second;
        }
        return std::unique_ptr<T>(static_cast<T*>(creator().release()));
    }

    /** @brief Returns true if prepare<T>() has been called for type T.
     *  @tparam T Queryable implementation type.
     *  @return True when T is registered. */
    template <typename T>
    bool isPrepared() const
    {
        std::shared_lock lock(mutex_);
        return creators_.count(std::type_index(typeid(T))) > 0;
    }

    /** @brief Creates a queryable instance for the given virtual table name.
     *  @param[in] table_name Virtual table name to look up.
     *  @return Queryable instance serving the named table.
     *  @throws std::runtime_error If no queryable is registered for the table. */
    IQueryableUPtr createByTable(std::string_view table_name)
    {
        std::function<IQueryableUPtr()> creator;
        {
            std::shared_lock lock(mutex_);
            const auto it = table_creators_.find(std::string(table_name));
            if (it == table_creators_.end())
            {
                throw std::runtime_error("QueryableFactory: table not prepared: " + std::string(table_name));
            }
            creator = it->second.creator;
        }
        return creator();
    }

    /** @brief Returns the names of all registered virtual tables.
     *  @return Set of virtual table name strings. */
    std::set<std::string> registeredTables() const
    {
        std::shared_lock lock(mutex_);
        std::set<std::string> tables;
        for (const auto& [table, registration] : table_creators_)
        {
            tables.insert(table);
        }
        return tables;
    }

    /** @brief Clears all registered creators and table mappings. Intended for test isolation only. */
    void reset()
    {
        std::unique_lock lock(mutex_);
        creators_.clear();
        table_creators_.clear();
    }

private:
    QueryableFactory() = default;
    mutable std::shared_mutex                                            mutex_;          ///< Guards creators_ and table_creators_.
    std::unordered_map<std::type_index, std::function<IQueryableUPtr()>> creators_;       ///< Creator functions keyed by type index.
    /** @brief Associates a creator function with the type that produced it. */
    struct TableCreator {
        std::function<IQueryableUPtr()> creator; ///< Factory function.
        std::type_index                 type;    ///< std::type_index of the registered queryable type.
    };
    std::unordered_map<std::string, TableCreator>                         table_creators_; ///< Creator functions and types keyed by virtual table name.
};

} // namespace mldp_pvxs_driver::query
