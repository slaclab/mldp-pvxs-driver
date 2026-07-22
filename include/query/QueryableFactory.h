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
#include <query/IQueryable.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <set>

namespace mldp_pvxs_driver::query {

class QueryableFactory
{
public:
    static QueryableFactory& instance()
    {
        static QueryableFactory inst;
        return inst;
    }

    // Startup: bind type T to its config. Must complete before any create<T>() call.
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

    // Runtime: create instance of T. Returns unique_ptr<T> directly.
    // static_cast is safe: the closure stored by prepare<T> always produces a T.
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

    template <typename T>
    bool isPrepared() const
    {
        std::shared_lock lock(mutex_);
        return creators_.count(std::type_index(typeid(T))) > 0;
    }

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

    // Test helper: clear all registered creators.
    void reset()
    {
        std::unique_lock lock(mutex_);
        creators_.clear();
        table_creators_.clear();
    }

private:
    QueryableFactory() = default;
    mutable std::shared_mutex                                            mutex_;
    std::unordered_map<std::type_index, std::function<IQueryableUPtr()>> creators_;
    struct TableCreator {
        std::function<IQueryableUPtr()> creator;
        std::type_index                 type;
    };
    std::unordered_map<std::string, TableCreator>                         table_creators_;
};

} // namespace mldp_pvxs_driver::query
