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

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::query {

class QueryCancelled final : public std::runtime_error
{
public:
    QueryCancelled()
        : std::runtime_error("Query cancelled")
    {
    }
};

class QueryCancellation
{
public:
    class Registration
    {
    public:
        Registration() = default;
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), id_(other.id_)
        {
        }
        Registration& operator=(Registration&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                id_ = other.id_;
            }
            return *this;
        }
        ~Registration() { reset(); }

        void reset()
        {
            if (owner_) owner_->remove(id_);
            owner_ = nullptr;
        }

    private:
        friend class QueryCancellation;
        Registration(QueryCancellation* owner, const std::size_t id)
            : owner_(owner), id_(id)
        {
        }
        QueryCancellation* owner_{nullptr};
        std::size_t        id_{0};
    };

    void requestCancel()
    {
        bool expected = false;
        if (!cancelled_.compare_exchange_strong(expected, true)) return;
        std::vector<std::function<void()>> callbacks;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [_, callback] : callbacks_) callbacks.push_back(callback);
        }
        for (const auto& callback : callbacks) callback();
    }

    [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(); }

    void throwIfCancelled() const
    {
        if (cancelled()) throw QueryCancelled{};
    }

    Registration onCancel(std::function<void()> callback)
    {
        bool invoke_now = false;
        std::size_t id = 0;
        {
            const std::lock_guard lock(mutex_);
            invoke_now = cancelled_.load();
            if (!invoke_now)
            {
                id = next_id_++;
                callbacks_.emplace(id, std::move(callback));
            }
        }
        if (invoke_now) callback();
        return Registration{this, id};
    }

private:
    void remove(const std::size_t id)
    {
        std::lock_guard lock(mutex_);
        callbacks_.erase(id);
    }

    std::atomic<bool>                       cancelled_{false};
    mutable std::mutex                      mutex_;
    std::size_t                               next_id_{1};
    std::map<std::size_t, std::function<void()>> callbacks_;
};

} // namespace mldp_pvxs_driver::query
