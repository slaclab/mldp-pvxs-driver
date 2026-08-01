//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/ExecutionContext.h>
#include <query/QueryExecutor.h>
#include <query/QueryPlanner.h>
#include <query/QueryProgress.h>
#include <query/QueryResult.h>
#include <query/QueryTableCatalog.h>
#include <query/QueryableFactory.h>
#include <query/SpillManager.h>
#include <query/executor/ExecutionState.h>
#include <query/parser/QueryParser.h>
#include <query/plan/PlannerError.h>

#include <config/Config.h>

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/array/builder_union.h>
#include <arrow/filesystem/mockfs.h>
#include <arrow/memory_pool.h>
#include <arrow/scalar.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

namespace plan = mldp_pvxs_driver::query::plan;

using namespace mldp_pvxs_driver;

namespace {

class FakeQueryable : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;
    inline static std::chrono::milliseconds execute_delay{0};

    explicit FakeQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view table_name) const override
    {
        if (table_name == "fake.paged")
        {
            return {
                {"pv", query::ColumnType::STRING, false, true, {}, {}, "Paged test value"},
            };
        }
        if (table_name == "fake.meta")
        {
            return {
                {"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"owner", query::ColumnType::STRING, false, true, {}, {query::PredicateOp::LIKE}, "owner"},
                {"tags", query::ColumnType::STRING, false, true, {}, {}, "tags"},
                {"attributes", query::ColumnType::STRING, false, true, {}, {}, "attributes"},
                {"tag", query::ColumnType::STRING, false, false, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "tag membership"},
            };
        }

        return {
            {"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
            {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
            {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::EQ, query::PredicateOp::IN}, "value"},
        };
    }

    query::QueryResult execute(std::string_view                     table_name,
                               const std::vector<query::Predicate>& pushable_predicates,
                               const std::set<std::string>&         projection_hint,
                               const query::ExecutionContext&,
                               std::string_view page_token = {}) override
    {
        if (execute_delay.count() > 0)
        {
            std::this_thread::sleep_for(execute_delay);
        }
        if (table_name == "fake.paged")
        {
            arrow::StringBuilder pv_builder;
            EXPECT_TRUE(pv_builder.Append(page_token.empty() ? "A" : "B").ok());
            std::shared_ptr<arrow::Array> pv;
            EXPECT_TRUE(pv_builder.Finish(&pv).ok());
            return query::QueryResult{
                .batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8())}), 1, {pv}),
                .next_page_token = page_token.empty() ? "second-page" : ""};
        }
        if (table_name == "fake.meta")
        {
            arrow::StringBuilder pv_builder;
            arrow::StringBuilder owner_builder;
            arrow::StringBuilder tags_builder;
            arrow::StringBuilder attributes_builder;
            arrow::StringBuilder device_group_builder;

            const auto include_row = [&](const std::string& pv) -> bool
            {
                for (const auto& predicate : pushable_predicates)
                {
                    if (predicate.column == "pv" &&
                        predicate.op == query::PredicateOp::EQ &&
                        !predicate.values.empty() &&
                        std::holds_alternative<std::string>(predicate.values.front()) &&
                        std::get<std::string>(predicate.values.front()) != pv)
                    {
                        return false;
                    }
                    if (predicate.column == "pv" && predicate.op == query::PredicateOp::IN)
                    {
                        bool matched = false;
                        for (const auto& value : predicate.values)
                        {
                            if (std::holds_alternative<std::string>(value) && std::get<std::string>(value) == pv)
                            {
                                matched = true;
                                break;
                            }
                        }
                        if (!matched)
                        {
                            return false;
                        }
                    }
                }
                return true;
            };

            if (include_row("A"))
            {
                EXPECT_TRUE(pv_builder.Append("A").ok());
                EXPECT_TRUE(owner_builder.Append("alice").ok());
                EXPECT_TRUE(tags_builder.Append("sample").ok());
                EXPECT_TRUE(attributes_builder.Append("device_group=RF").ok());
                EXPECT_TRUE(device_group_builder.Append("RF").ok());
            }
            if (include_row("C"))
            {
                EXPECT_TRUE(pv_builder.Append("C").ok());
                EXPECT_TRUE(owner_builder.Append("carol").ok());
                EXPECT_TRUE(tags_builder.Append("configuration").ok());
                EXPECT_TRUE(attributes_builder.Append("device_group=MAGNET").ok());
                EXPECT_TRUE(device_group_builder.Append("MAGNET").ok());
            }

            std::shared_ptr<arrow::Array> pv;
            std::shared_ptr<arrow::Array> owner;
            std::shared_ptr<arrow::Array> tags;
            std::shared_ptr<arrow::Array> attributes;
            std::shared_ptr<arrow::Array> device_group;
            EXPECT_TRUE(pv_builder.Finish(&pv).ok());
            EXPECT_TRUE(owner_builder.Finish(&owner).ok());
            EXPECT_TRUE(tags_builder.Finish(&tags).ok());
            EXPECT_TRUE(attributes_builder.Finish(&attributes).ok());
            EXPECT_TRUE(device_group_builder.Finish(&device_group).ok());

            std::vector<std::shared_ptr<arrow::Field>> fields;
            std::vector<std::shared_ptr<arrow::Array>> columns;
            if (projection_hint.empty() || projection_hint.contains("pv"))
            {
                fields.push_back(arrow::field("pv", arrow::utf8()));
                columns.push_back(pv);
            }
            if (projection_hint.empty() || projection_hint.contains("owner"))
            {
                fields.push_back(arrow::field("owner", arrow::utf8()));
                columns.push_back(owner);
            }
            if (projection_hint.empty() || projection_hint.contains("tags"))
            {
                fields.push_back(arrow::field("tags", arrow::utf8()));
                columns.push_back(tags);
            }
            if (projection_hint.empty() || projection_hint.contains("attributes"))
            {
                fields.push_back(arrow::field("attributes", arrow::utf8()));
                columns.push_back(attributes);
            }
            if (projection_hint.empty() || projection_hint.contains("attributes.device_group"))
            {
                fields.push_back(arrow::field("attributes.device_group", arrow::utf8()));
                columns.push_back(device_group);
            }

            return query::QueryResult{
                .batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), pv->length(), std::move(columns)),
                .next_page_token = ""};
        }

        arrow::StringBuilder pv_builder;
        arrow::Int64Builder  time_builder;
        arrow::Int64Builder  value_builder;

        const auto include_row = [&](const std::string& pv, const int64_t time) -> bool
        {
            for (const auto& predicate : pushable_predicates)
            {
                if (predicate.column == "pv" &&
                    predicate.op == query::PredicateOp::EQ &&
                    !predicate.values.empty() &&
                    std::holds_alternative<std::string>(predicate.values.front()) &&
                    std::get<std::string>(predicate.values.front()) != pv)
                {
                    return false;
                }
                if (predicate.column == "time" &&
                    predicate.op == query::PredicateOp::GTE &&
                    !predicate.values.empty() &&
                    std::holds_alternative<int64_t>(predicate.values.front()) &&
                    time < std::get<int64_t>(predicate.values.front()))
                {
                    return false;
                }
                if (predicate.column == "pv" && predicate.op == query::PredicateOp::IN)
                {
                    bool matched = false;
                    for (const auto& value : predicate.values)
                    {
                        if (std::holds_alternative<std::string>(value) && std::get<std::string>(value) == pv)
                        {
                            matched = true;
                            break;
                        }
                    }
                    if (!matched)
                        return false;
                }
            }
            return true;
        };

        if (include_row("A", 10))
        {
            EXPECT_TRUE(pv_builder.Append("A").ok());
            EXPECT_TRUE(time_builder.Append(10).ok());
            EXPECT_TRUE(value_builder.Append(1).ok());
        }
        if (include_row("B", 20))
        {
            EXPECT_TRUE(pv_builder.Append("B").ok());
            EXPECT_TRUE(time_builder.Append(20).ok());
            EXPECT_TRUE(value_builder.Append(2).ok());
        }

        std::shared_ptr<arrow::Array> pv;
        std::shared_ptr<arrow::Array> time;
        std::shared_ptr<arrow::Array> value;
        EXPECT_TRUE(pv_builder.Finish(&pv).ok());
        EXPECT_TRUE(time_builder.Finish(&time).ok());
        EXPECT_TRUE(value_builder.Finish(&value).ok());

        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> columns;
        if (projection_hint.empty() || projection_hint.contains("pv"))
        {
            fields.push_back(arrow::field("pv", arrow::utf8()));
            columns.push_back(pv);
        }
        if (projection_hint.empty() || projection_hint.contains("time"))
        {
            fields.push_back(arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND, "UTC")));
            columns.push_back(time);
        }
        if (projection_hint.empty() || projection_hint.contains("value"))
        {
            fields.push_back(arrow::field("value", arrow::int64()));
            columns.push_back(value);
        }

        return query::QueryResult{
            .batch = arrow::RecordBatch::Make(arrow::schema(std::move(fields)), pv->length(), std::move(columns)),
            .next_page_token = ""};
    }
};

class EmptyWideInputQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view>                  kVirtualTables;
    inline static uint64_t                                   execute_calls{0};
    inline static std::vector<std::vector<query::Predicate>> received_predicates;

    explicit EmptyWideInputQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(const std::string_view table_name) const override
    {
        if (table_name == "mldp.pv_metadata")
        {
            return {{"pv", query::ColumnType::STRING, false, true, {query::PredicateOp::EQ}, {}, "PV"}};
        }
        if (table_name == "mldp.configuration_activation")
        {
            return {
                {"time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "time"},
                {"end_time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "end time"},
                {"activation_id", query::ColumnType::STRING, false, true, {query::PredicateOp::EQ}, {}, "activation id"},
            };
        }
        return {
            {"pv", query::ColumnType::STRING, true, false, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV input"},
            {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
            {"window", query::ColumnType::TIMESTAMP, false, false, {query::PredicateOp::IN}, {}, "window input"},
        };
    }

    query::QueryResult execute(std::string_view                     table_name,
                               const std::vector<query::Predicate>& predicates,
                               const std::set<std::string>&,
                               const query::ExecutionContext&,
                               std::string_view = {}) override
    {
        ++execute_calls;
        received_predicates.push_back(predicates);
        if (table_name == "mldp.time_series")
        {
            arrow::StringBuilder    pv_builder;
            arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
            int64_t                 begin_seconds = std::numeric_limits<int64_t>::min();
            int64_t                 end_seconds = std::numeric_limits<int64_t>::max();
            for (const auto& predicate : predicates)
            {
                if (predicate.column == "time" && predicate.op == query::PredicateOp::GTE)
                    begin_seconds = std::get<int64_t>(predicate.values.front());
                if (predicate.column == "time" && predicate.op == query::PredicateOp::LTE)
                    end_seconds = std::get<int64_t>(predicate.values.front());
            }
            for (const int64_t timestamp : {0LL, 5'000'000'000LL, 10'000'000'000LL})
            {
                const auto seconds = timestamp / 1'000'000'000LL;
                if (seconds < begin_seconds || seconds > end_seconds)
                    continue;
                EXPECT_TRUE(pv_builder.Append("PV:ONE").ok());
                EXPECT_TRUE(time_builder.Append(timestamp).ok());
            }
            std::shared_ptr<arrow::Array> pv;
            std::shared_ptr<arrow::Array> time;
            EXPECT_TRUE(pv_builder.Finish(&pv).ok());
            EXPECT_TRUE(time_builder.Finish(&time).ok());
            return {.batch = arrow::RecordBatch::Make(
                        arrow::schema({arrow::field("pv", pv->type()), arrow::field("time", time->type())}),
                        pv->length(), {pv, time})};
        }
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "activation_id" || predicate.column == "pv")
            {
                return {};
            }
        }
        return {};
    }
};

class NativeCreateQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;
    inline static uint64_t                  stream_creations{0};
    inline static uint64_t                  next_calls{0};

    explicit NativeCreateQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr) {}

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
                {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::IN}, "value"}};
    }

    query::QueryResult execute(std::string_view, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        throw std::runtime_error("NativeCreateQueryable requires executeStream");
    }

    query::IRecordBatchStreamUPtr executeStream(std::string_view, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        ++stream_creations;

        class Stream final : public query::IRecordBatchStream
        {
        public:
            std::shared_ptr<arrow::RecordBatch> next() override
            {
                ++NativeCreateQueryable::next_calls;
                if (index_ == 3)
                    return nullptr;
                arrow::StringBuilder    pv;
                arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                arrow::Int64Builder     value;
                const auto              row = static_cast<int64_t>(++index_);
                if (!pv.Append("CREATE:PV").ok() || !time.Append(row * 1'000'000'000LL).ok() || !value.Append(row).ok())
                    throw std::runtime_error("Failed to build native CREATE batch");
                std::shared_ptr<arrow::Array> pv_array;
                std::shared_ptr<arrow::Array> time_array;
                std::shared_ptr<arrow::Array> value_array;
                if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
                    throw std::runtime_error("Failed to finish native CREATE batch");
                return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}),
                                                1, {pv_array, time_array, value_array});
            }

        private:
            uint64_t index_{0};
        };

        return std::make_unique<Stream>();
    }
};

class SubqueryWindowQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view>                  kVirtualTables;
    inline static std::vector<std::vector<query::Predicate>> requests;
    inline static std::mutex                                 requests_mutex;

    explicit SubqueryWindowQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr) {}

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(const std::string_view table_name) const override
    {
        if (table_name == "mldp.configuration_activation")
            return {{"time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "start"},
                    {"end_time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "end"}};
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
                {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::IN}, "value"}};
    }

    query::QueryResult execute(const std::string_view table_name, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        if (table_name != "mldp.configuration_activation")
            throw std::runtime_error("SubqueryWindowQueryable requires executeStream for time series");
        arrow::TimestampBuilder start(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::TimestampBuilder end(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        if (!start.Append(0).ok() || !start.Append(10'000'000'000LL).ok() || !end.Append(5'000'000'000LL).ok() || !end.Append(15'000'000'000LL).ok())
            throw std::runtime_error("Failed to build window subquery");
        std::shared_ptr<arrow::Array> starts;
        std::shared_ptr<arrow::Array> ends;
        if (!start.Finish(&starts).ok() || !end.Finish(&ends).ok())
            throw std::runtime_error("Failed to finish window subquery");
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("time", starts->type()), arrow::field("end_time", ends->type())}), 2, {starts, ends})};
    }

    query::IRecordBatchStreamUPtr executeStream(std::string_view, const std::vector<query::Predicate>& predicates, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        {
            const std::lock_guard lock(requests_mutex);
            requests.push_back(predicates);
        }

        class Stream final : public query::IRecordBatchStream
        {
        public:
            explicit Stream(const std::vector<query::Predicate>& predicates)
            {
                for (const auto& predicate : predicates)
                {
                    if (predicate.column == "pv")
                        pv_ = std::get<std::string>(predicate.values.front());
                    if (predicate.column == "time" && predicate.op == query::PredicateOp::GTE)
                        begin_ = std::get<int64_t>(predicate.values.front());
                    if (predicate.column == "time" && predicate.op == query::PredicateOp::LTE)
                        end_ = std::get<int64_t>(predicate.values.front());
                }
            }

            std::shared_ptr<arrow::RecordBatch> next() override
            {
                if (sent_)
                    return nullptr;
                sent_ = true;
                arrow::StringBuilder    pv;
                arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                arrow::Int64Builder     value;
                if (!pv.Append(pv_).ok() || !time.Append((begin_ + 1) * 1'000'000'000LL).ok() || !value.Append(begin_ + 1).ok())
                    throw std::runtime_error("Failed to build subquery window batch");
                std::shared_ptr<arrow::Array> pv_array;
                std::shared_ptr<arrow::Array> time_array;
                std::shared_ptr<arrow::Array> value_array;
                if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
                    throw std::runtime_error("Failed to finish subquery window batch");
                return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}), 1, {pv_array, time_array, value_array});
            }

        private:
            std::string pv_;
            int64_t     begin_{0};
            int64_t     end_{0};
            bool        sent_{false};
        };

        return std::make_unique<Stream>(predicates);
    }
};

class WideStreamingQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;

    explicit WideStreamingQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr) {}

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
                {"value", query::ColumnType::INT, false, true, {}, {}, "value"}};
    }

    query::QueryResult execute(std::string_view, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        throw std::runtime_error("WideStreamingQueryable requires executeStream");
    }

    query::IRecordBatchStreamUPtr executeStream(std::string_view, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        class Stream final : public query::IRecordBatchStream
        {
        public:
            std::shared_ptr<arrow::RecordBatch> next() override
            {
                if (index_ == 2)
                    return nullptr;
                const std::array<std::array<std::string_view, 2>, 2> pvs{{{{"WIDE:TWO", "WIDE:ONE"}}, {{"WIDE:ONE", "WIDE:TWO"}}}};
                const std::array<std::array<int64_t, 2>, 2>          times{{{{1, 0}}, {{1, 0}}}};
                const std::array<std::array<int64_t, 2>, 2>          values{{{{21, 10}}, {{11, 20}}}};
                arrow::StringBuilder                                 pv;
                arrow::TimestampBuilder                              time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                arrow::Int64Builder                                  value;
                for (std::size_t row = 0; row < 2; ++row)
                {
                    if (!pv.Append(pvs[index_][row]).ok() || !time.Append(times[index_][row] * 1'000'000'000LL).ok() || !value.Append(values[index_][row]).ok())
                        throw std::runtime_error("Failed to build wide stream batch");
                }
                ++index_;
                std::shared_ptr<arrow::Array> pv_array;
                std::shared_ptr<arrow::Array> time_array;
                std::shared_ptr<arrow::Array> value_array;
                if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
                    throw std::runtime_error("Failed to finish wide stream batch");
                return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}), 2, {pv_array, time_array, value_array});
            }

        private:
            std::size_t index_{0};
        };

        return std::make_unique<Stream>();
    }
};

class ConcurrentWindowQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view> kVirtualTables;
    inline static std::mutex                mutex;
    inline static std::condition_variable   condition;
    inline static uint64_t                  active_streams{0};
    inline static uint64_t                  peak_active_streams{0};
    inline static uint64_t                  started_streams{0};

    explicit ConcurrentWindowQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr) {}

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::size_t maxConcurrentStreams() const noexcept override
    {
        return 2;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
                {"value", query::ColumnType::INT, false, true, {}, {}, "value"}};
    }

    query::QueryResult execute(std::string_view, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        throw std::runtime_error("ConcurrentWindowQueryable requires executeStream");
    }

    query::IRecordBatchStreamUPtr executeStream(std::string_view, const std::vector<query::Predicate>& predicates, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        std::string pv;
        for (const auto& predicate : predicates)
            if (predicate.column == "pv")
                pv = std::get<std::string>(predicate.values.front());

        class Stream final : public query::IRecordBatchStream
        {
        public:
            explicit Stream(std::string pv) : pv_(std::move(pv)) {}

            ~Stream() override
            {
                if (active_)
                {
                    const std::lock_guard lock(ConcurrentWindowQueryable::mutex);
                    --ConcurrentWindowQueryable::active_streams;
                    ConcurrentWindowQueryable::condition.notify_all();
                }
            }

            std::shared_ptr<arrow::RecordBatch> next() override
            {
                if (sent_)
                    return nullptr;
                {
                    std::unique_lock lock(ConcurrentWindowQueryable::mutex);
                    if (!active_)
                    {
                        active_ = true;
                        ++ConcurrentWindowQueryable::started_streams;
                        ++ConcurrentWindowQueryable::active_streams;
                        ConcurrentWindowQueryable::peak_active_streams = std::max(ConcurrentWindowQueryable::peak_active_streams,
                                                                                  ConcurrentWindowQueryable::active_streams);
                        ConcurrentWindowQueryable::condition.notify_all();
                    }
                    ConcurrentWindowQueryable::condition.wait(lock, []
                                                              {
                                                                  return ConcurrentWindowQueryable::started_streams >= 2;
                                                              });
                }
                sent_ = true;
                arrow::StringBuilder    pv;
                arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                arrow::Int64Builder     value;
                if (!pv.Append(pv_).ok() || !time.Append(1'000'000'000LL).ok() || !value.Append(pv_.back() - '0').ok())
                    throw std::runtime_error("Failed to build concurrent window batch");
                std::shared_ptr<arrow::Array> pv_array;
                std::shared_ptr<arrow::Array> time_array;
                std::shared_ptr<arrow::Array> value_array;
                if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
                    throw std::runtime_error("Failed to finish concurrent window batch");
                return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}),
                                                1, {pv_array, time_array, value_array});
            }

        private:
            std::string pv_;
            bool        active_{false};
            bool        sent_{false};
        };

        return std::make_unique<Stream>(std::move(pv));
    }
};

class ScaledWindowQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view>                  kVirtualTables;
    inline static std::mutex                                 mutex;
    inline static std::condition_variable                    condition;
    inline static uint64_t                                   active_streams{0};
    inline static uint64_t                                   peak_active_streams{0};
    inline static uint64_t                                   started_streams{0};
    inline static uint64_t                                   metadata_execute_calls{0};
    inline static uint64_t                                   time_series_execute_calls{0};
    inline static bool                                       require_parallel_wave{false};
    inline static bool                                       fail_stream_open{false};
    inline static std::size_t                                emitted_pv_count{std::numeric_limits<std::size_t>::max()};
    inline static std::vector<std::vector<query::Predicate>> requests;

    explicit ScaledWindowQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr) {}

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::size_t maxConcurrentStreams() const noexcept override
    {
        return 4;
    }

    std::vector<query::ColumnSchema> tableSchema(const std::string_view table_name) const override
    {
        if (table_name == "mldp.pv_metadata")
            return {{"pv", query::ColumnType::STRING, false, true, {}, {}, "PV"},
                    {"attributes.dname", query::ColumnType::STRING, false, true, {}, {query::PredicateOp::PREFIX}, "display name"},
                    {"value", query::ColumnType::INT, false, true, {}, {}, "metadata value"}};
        if (table_name == "mldp.configuration_activation")
            return {{"time", query::ColumnType::TIMESTAMP, false, true, {}, {}, "activation time"},
                    {"config_name", query::ColumnType::STRING, false, true, {query::PredicateOp::EQ}, {}, "configuration name"}};
        return {{"pv", query::ColumnType::STRING, true, true, {query::PredicateOp::EQ, query::PredicateOp::IN}, {}, "PV"},
                {"time", query::ColumnType::TIMESTAMP, false, true, {query::PredicateOp::GTE, query::PredicateOp::LTE}, {}, "time"},
                {"value", query::ColumnType::INT, false, true, {}, {query::PredicateOp::IN}, "value"}};
    }

    query::QueryResult execute(const std::string_view table_name, const std::vector<query::Predicate>&, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        if (table_name == "mldp.pv_metadata")
        {
            ++metadata_execute_calls;
            arrow::StringBuilder pv;
            arrow::StringBuilder dname;
            arrow::Int64Builder  value;
            for (const auto& name : pvs())
            {
                if (!pv.Append(name).ok() || !dname.Append(name).ok() || !value.Append(1).ok())
                    throw std::runtime_error("Failed to build scaled metadata batch");
            }
            std::shared_ptr<arrow::Array> pv_array;
            std::shared_ptr<arrow::Array> dname_array;
            std::shared_ptr<arrow::Array> value_array;
            if (!pv.Finish(&pv_array).ok() || !dname.Finish(&dname_array).ok() || !value.Finish(&value_array).ok())
                throw std::runtime_error("Failed to finish scaled metadata batch");
            return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("attributes.dname", dname_array->type()), arrow::field("value", value_array->type())}),
                                                      pv_array->length(), {pv_array, dname_array, value_array})};
        }
        if (table_name == "mldp.configuration_activation")
        {
            arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
            arrow::StringBuilder    config_name;
            if (!time.Append(0).ok() || !config_name.Append("SPEAR User").ok())
                throw std::runtime_error("Failed to build scaled activation batch");
            std::shared_ptr<arrow::Array> time_array;
            std::shared_ptr<arrow::Array> config_name_array;
            if (!time.Finish(&time_array).ok() || !config_name.Finish(&config_name_array).ok())
                throw std::runtime_error("Failed to finish scaled activation batch");
            return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("time", time_array->type()), arrow::field("config_name", config_name_array->type())}),
                                                      1, {time_array, config_name_array})};
        }
        ++time_series_execute_calls;
        arrow::StringBuilder    pv;
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
        arrow::Int64Builder     value;
        if (!pv.Append("PV:01").ok() || !time.Append(1'000'000'000LL).ok() || !value.Append(1).ok())
            throw std::runtime_error("Failed to build materialized scaled time-series batch");
        std::shared_ptr<arrow::Array> pv_array;
        std::shared_ptr<arrow::Array> time_array;
        std::shared_ptr<arrow::Array> value_array;
        if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
            throw std::runtime_error("Failed to finish materialized scaled time-series batch");
        return {.batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}),
                                                  1, {pv_array, time_array, value_array})};
    }

    query::IRecordBatchStreamUPtr executeStream(std::string_view, const std::vector<query::Predicate>& predicates, const std::set<std::string>&, const query::ExecutionContext&, std::string_view = {}) override
    {
        std::vector<std::string> shard_pvs;
        int64_t                  begin_seconds = 0;
        int64_t                  end_seconds = 0;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "pv")
                for (const auto& value : predicate.values)
                    shard_pvs.push_back(std::get<std::string>(value));
            if (predicate.column == "time" && predicate.op == query::PredicateOp::GTE)
                begin_seconds = std::get<int64_t>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::LTE)
                end_seconds = std::get<int64_t>(predicate.values.front());
        }
        {
            std::unique_lock lock(mutex);
            requests.push_back(predicates);
            ++started_streams;
            ++active_streams;
            peak_active_streams = std::max(peak_active_streams, active_streams);
            condition.notify_all();
            if (require_parallel_wave)
            {
                const auto opened_parallel_wave = condition.wait_for(lock, std::chrono::seconds(1), []
                                                                     {
                                                                         return ScaledWindowQueryable::started_streams >= 4;
                                                                     });
                if (!opened_parallel_wave)
                    throw std::runtime_error("Scaled window query did not open four parallel series shards");
            }
        }
        if (fail_stream_open)
            throw std::invalid_argument("Invalid argument");

        const auto emitted_pv_count = ScaledWindowQueryable::emitted_pv_count;
        class Stream final : public query::IRecordBatchStream
        {
        public:
            Stream(std::vector<std::string> pvs, const int64_t begin_seconds, const int64_t end_seconds, const std::size_t emitted_pv_count)
                : pvs_(std::move(pvs)), begin_seconds_(begin_seconds), end_seconds_(end_seconds), emitted_pv_count_(emitted_pv_count), active_(true) {}

            ~Stream() override
            {
                if (!active_)
                    return;
                const std::lock_guard lock(ScaledWindowQueryable::mutex);
                --ScaledWindowQueryable::active_streams;
                ScaledWindowQueryable::condition.notify_all();
            }

            std::shared_ptr<arrow::RecordBatch> next() override
            {
                if (sent_)
                    return nullptr;
                sent_ = true;
                arrow::StringBuilder     pv;
                arrow::TimestampBuilder  time(arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), arrow::default_memory_pool());
                const auto               value_type = arrow::dense_union({arrow::field("int64", arrow::int64())});
                auto                     int64_values = std::make_shared<arrow::Int64Builder>();
                arrow::DenseUnionBuilder value(arrow::default_memory_pool(), {int64_values}, value_type);
                for (std::size_t index = 0; index < std::min(pvs_.size(), emitted_pv_count_); ++index)
                {
                    const auto& name = pvs_[index];
                    for (const auto seconds : {begin_seconds_ - 1, begin_seconds_ + 1, end_seconds_})
                    {
                        if (!pv.Append(name).ok() || !time.Append(seconds * 1'000'000'000LL).ok() ||
                            !value.Append(0).ok() || !int64_values->Append(1).ok())
                            throw std::runtime_error("Failed to build scaled time-series batch");
                    }
                }
                std::shared_ptr<arrow::Array> pv_array;
                std::shared_ptr<arrow::Array> time_array;
                std::shared_ptr<arrow::Array> value_array;
                if (!pv.Finish(&pv_array).ok() || !time.Finish(&time_array).ok() || !value.Finish(&value_array).ok())
                    throw std::runtime_error("Failed to finish scaled time-series batch");
                return arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", pv_array->type()), arrow::field("time", time_array->type()), arrow::field("value", value_array->type())}),
                                                pv_array->length(), {pv_array, time_array, value_array});
            }

        private:
            std::vector<std::string> pvs_;
            int64_t                  begin_seconds_{0};
            int64_t                  end_seconds_{0};
            std::size_t              emitted_pv_count_{0};
            bool                     active_{false};
            bool                     sent_{false};
        };

        return std::make_unique<Stream>(std::move(shard_pvs), begin_seconds, end_seconds, emitted_pv_count);
    }

    static const std::vector<std::string>& pvs()
    {
        static const std::vector<std::string> values{
            "USEG:UNDH:1450:GapAct", "USEG:UNDH:1550:GapAct", "USEG:UNDH:1650:GapAct", "USEG:UNDH:1750:GapAct",
            "USEG:UNDH:1850:GapAct", "USEG:UNDH:1950:GapAct", "USEG:UNDH:2050:GapAct", "USEG:UNDH:2250:GapAct",
            "USEG:UNDH:2350:GapAct", "USEG:UNDH:2450:GapAct", "USEG:UNDH:2550:GapAct", "USEG:UNDH:2650:GapAct",
            "USEG:UNDH:2750:GapAct", "USEG:UNDH:2950:GapAct", "USEG:UNDH:3050:GapAct", "USEG:UNDH:3150:GapAct",
            "USEG:UNDH:3250:GapAct", "USEG:UNDH:3350:GapAct", "USEG:UNDH:3450:GapAct", "USEG:UNDH:3550:GapAct",
            "USEG:UNDH:3650:GapAct", "USEG:UNDH:3750:GapAct", "USEG:UNDH:3850:GapAct", "USEG:UNDH:3950:GapAct",
            "USEG:UNDH:4050:GapAct", "USEG:UNDH:4150:GapAct", "USEG:UNDH:4250:GapAct", "USEG:UNDH:4350:GapAct",
            "USEG:UNDH:4450:GapAct", "USEG:UNDH:4550:GapAct", "USEG:UNDH:4650:GapAct", "USEG:UNDH:4750:GapAct"};
        return values;
    }
};

const std::set<std::string_view> FakeQueryable::kVirtualTables = {"fake.samples", "fake.meta", "fake.paged"};
const std::set<std::string_view> ConcurrentWindowQueryable::kVirtualTables = {"mldp.time_series"};
const std::set<std::string_view> ScaledWindowQueryable::kVirtualTables = {
    "mldp.time_series", "mldp.time_series_table", "mldp.pv_metadata", "mldp.configuration_activation"};
const std::set<std::string_view> EmptyWideInputQueryable::kVirtualTables = {
    "mldp.time_series", "mldp.time_series_table", "mldp.pv_metadata", "mldp.configuration_activation"};
const std::set<std::string_view> NativeCreateQueryable::kVirtualTables = {"mldp.time_series"};
const std::set<std::string_view> SubqueryWindowQueryable::kVirtualTables = {"mldp.time_series", "mldp.time_series_table", "mldp.configuration_activation"};
const std::set<std::string_view> WideStreamingQueryable::kVirtualTables = {"mldp.time_series", "mldp.time_series_table"};

class ActivationTimestampQueryable final : public query::IQueryable
{
public:
    static const std::set<std::string_view>     kVirtualTables;
    inline static std::vector<query::Predicate> received_predicates;

    explicit ActivationTimestampQueryable(const config::Config&, std::shared_ptr<metrics::Metrics> = nullptr)
    {
    }

    std::set<std::string_view> virtualTables() const override
    {
        return kVirtualTables;
    }

    std::vector<query::ColumnSchema> tableSchema(std::string_view) const override
    {
        const auto timestamp_ops = std::set<query::PredicateOp>{query::PredicateOp::EQ, query::PredicateOp::NEQ, query::PredicateOp::LT, query::PredicateOp::LTE, query::PredicateOp::GT, query::PredicateOp::GTE};
        const auto end_time_ops = std::set<query::PredicateOp>{query::PredicateOp::EQ, query::PredicateOp::NEQ, query::PredicateOp::LT, query::PredicateOp::LTE, query::PredicateOp::GT, query::PredicateOp::GTE, query::PredicateOp::IS_NULL, query::PredicateOp::IS_NOT_NULL};
        return {{"time", query::ColumnType::TIMESTAMP, false, true, timestamp_ops, timestamp_ops, "Activation start"},
                {"end_time", query::ColumnType::TIMESTAMP, false, true, end_time_ops, end_time_ops, "Activation end"}};
    }

    query::QueryResult execute(std::string_view,
                               const std::vector<query::Predicate>& predicates,
                               const std::set<std::string>&,
                               const query::ExecutionContext&,
                               std::string_view = {}) override
    {
        received_predicates = predicates;
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
        arrow::TimestampBuilder end_time(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
        EXPECT_TRUE(time.Append(10'000'000'000LL).ok());
        EXPECT_TRUE(time.Append(5'000'000'000LL).ok());
        EXPECT_TRUE(end_time.Append(20'000'000'000LL).ok());
        EXPECT_TRUE(end_time.AppendNull().ok());
        std::shared_ptr<arrow::Array> starts;
        std::shared_ptr<arrow::Array> ends;
        EXPECT_TRUE(time.Finish(&starts).ok());
        EXPECT_TRUE(end_time.Finish(&ends).ok());
        return {.batch = arrow::RecordBatch::Make(
                    arrow::schema({arrow::field("time", starts->type()), arrow::field("end_time", ends->type())}),
                    starts->length(), {starts, ends})};
    }
};

const std::set<std::string_view> ActivationTimestampQueryable::kVirtualTables = {"mldp.configuration_activation"};

class PlannerExecutorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        query::QueryableFactory::instance().reset();
        query::QueryableFactory::instance().prepare<FakeQueryable>(config::Config::configFromYamlString("{}"));
    }

    void TearDown() override
    {
        FakeQueryable::execute_delay = std::chrono::milliseconds{0};
        query::QueryableFactory::instance().reset();
    }
};

TEST(EmptyWideInputTest, EmptyValidSubqueriesProduceNoWideTableRequest)
{
    query::QueryableFactory::instance().reset();
    EmptyWideInputQueryable::execute_calls = 0;
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           pv_empty = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT * FROM mldp.time_series_table WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv = 'NO:PV')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(pv_empty.batches.empty());
    EXPECT_EQ(pv_empty.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);

    EmptyWideInputQueryable::execute_calls = 0;
    const auto window_empty = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT * FROM mldp.time_series_table WHERE pv = 'PV:ONE' " "AND window IN (SELECT time, end_time FROM mldp.configuration_activation WHERE activation_id = 'none')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(window_empty.batches.empty());
    EXPECT_EQ(window_empty.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);

    EmptyWideInputQueryable::execute_calls = 0;
    const auto long_window_empty = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT pv, time FROM mldp.time_series WHERE pv = 'PV:ONE' " "AND window IN (SELECT time, end_time FROM mldp.configuration_activation WHERE activation_id = 'none')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(long_window_empty.batches.empty());
    EXPECT_EQ(long_window_empty.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);
    query::QueryableFactory::instance().reset();
}

TEST(EmptyWideInputTest, EmptyPvSubqueryProducesNoNarrowTimeSeriesRequest)
{
    query::QueryableFactory::instance().reset();
    EmptyWideInputQueryable::execute_calls = 0;
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT pv, time FROM mldp.time_series " "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE pv = 'NO:PV')")),
        {.pool = arrow::default_memory_pool()});
    EXPECT_TRUE(result.batches.empty());
    EXPECT_EQ(result.stats.rpc_calls, 1U);
    EXPECT_EQ(EmptyWideInputQueryable::execute_calls, 1U);
    query::QueryableFactory::instance().reset();
}

const plan::PhysicalTableScan* findScan(const plan::PhysicalNodePtr& node)
{
    if (!node)
    {
        return nullptr;
    }
    if (const auto* scan = std::get_if<plan::PhysicalTableScan>(&node->value))
    {
        return scan;
    }
    if (const auto* filter = std::get_if<plan::PhysicalFilter>(&node->value))
    {
        return findScan(filter->input);
    }
    if (const auto* project = std::get_if<plan::PhysicalProject>(&node->value))
    {
        return findScan(project->input);
    }
    if (const auto* sort = std::get_if<plan::PhysicalSort>(&node->value))
    {
        return findScan(sort->input);
    }
    if (const auto* limit = std::get_if<plan::PhysicalLimit>(&node->value))
    {
        return findScan(limit->input);
    }
    return nullptr;
}

TEST(QueryPlannerTest, ResolvesDefaultAndExplicitWindowShardOptions)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner planner;
    const auto          defaults = findScan(planner.plan(query::parseQuery(
        "SELECT * FROM mldp.time_series WHERE pv = 'PV:ONE' AND window IN (1700000000, 1700000010)")));
    ASSERT_NE(defaults, nullptr);
    EXPECT_EQ(defaults->window_shards.slice_ns, 1'000'000'000LL);
    EXPECT_EQ(defaults->window_shards.series_per_shard, 1U);

    const auto explicit_options = findScan(planner.plan(query::parseQuery(
        "SELECT * FROM mldp.time_series WHERE pv IN ('PV:ONE', 'PV:TWO') " "AND window IN (1700000000, 1700000010; slice 5s, series_per_shard 2)")));
    ASSERT_NE(explicit_options, nullptr);
    EXPECT_EQ(explicit_options->window_shards.slice_ns, 5'000'000'000LL);
    EXPECT_EQ(explicit_options->window_shards.series_per_shard, 2U);
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, VisitsWindowShardsSeriallyBySliceThenPvGroup)
{
    query::QueryableFactory::instance().reset();
    EmptyWideInputQueryable::execute_calls = 0;
    EmptyWideInputQueryable::received_predicates.clear();
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    (void)executor.execute(planner.plan(query::parseQuery(
                               "SELECT pv, time FROM mldp.time_series WHERE pv IN ('PV:ONE', 'PV:TWO') " "AND window IN (0, 10; slice 5s, series_per_shard 1)")),
                           {.pool = arrow::default_memory_pool()});

    ASSERT_EQ(EmptyWideInputQueryable::received_predicates.size(), 4U);
    const auto shard = [](const std::vector<query::Predicate>& predicates)
    {
        std::string pv;
        int64_t     begin = -1;
        int64_t     end = -1;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "pv")
                pv = std::get<std::string>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::GTE)
                begin = std::get<int64_t>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::LTE)
                end = std::get<int64_t>(predicate.values.front());
        }
        return std::tuple{pv, begin, end};
    };
    EXPECT_EQ(shard(EmptyWideInputQueryable::received_predicates[0]), (std::tuple{"PV:ONE", 0LL, 5LL}));
    EXPECT_EQ(shard(EmptyWideInputQueryable::received_predicates[1]), (std::tuple{"PV:TWO", 0LL, 5LL}));
    EXPECT_EQ(shard(EmptyWideInputQueryable::received_predicates[2]), (std::tuple{"PV:ONE", 5LL, 10LL}));
    EXPECT_EQ(shard(EmptyWideInputQueryable::received_predicates[3]), (std::tuple{"PV:TWO", 5LL, 10LL}));
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, FiltersDuplicateInclusiveSliceBoundaries)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(planner.plan(query::parseQuery(
                                                       "SELECT pv, time FROM mldp.time_series WHERE pv = 'PV:ONE' AND window IN (0, 10; slice 5s)")),
                                                   {.pool = arrow::default_memory_pool()});
    std::vector<int64_t> timestamps;
    for (const auto& batch : result.batches)
    {
        const auto times = std::static_pointer_cast<arrow::TimestampArray>(batch->GetColumnByName("time"));
        for (int64_t index = 0; index < times->length(); ++index)
            timestamps.push_back(times->Value(index));
    }
    EXPECT_EQ(timestamps, (std::vector<int64_t>{0LL, 5'000'000'000LL, 10'000'000'000LL}));
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerTest, RejectsInvalidWindowShardOptions)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<EmptyWideInputQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner planner;
    for (const std::string_view sql : {
             "SELECT * FROM mldp.time_series WHERE pv = 'PV:ONE' AND window IN (1, 2; slice 0s)",
             "SELECT * FROM mldp.time_series WHERE pv = 'PV:ONE' AND window IN (1, 2; series_per_shard 0)",
             "SELECT * FROM mldp.time_series WHERE pv = 'PV:ONE' AND window IN (1, 2; slice 1s, slice 2s)",
             "SELECT * FROM mldp.time_series WHERE pv = 'PV:ONE' AND window IN (1, 2; unknown 1)"})
    {
        EXPECT_THROW((void)planner.plan(query::parseQuery(sql)), query::plan::PlannerException) << sql;
    }
    query::QueryableFactory::instance().reset();
}

TEST_F(PlannerExecutorTest, PlansSelectWithBackboneNodes)
{
    query::QueryPlanner planner;
    const auto          statement = query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' LIMIT 1");
    const auto          plan = planner.plan(statement);
    const auto          text = query::plan::physicalPlanToString(plan);
    EXPECT_NE(text.find("PhysicalLimit"), std::string::npos);
    EXPECT_NE(text.find("PhysicalProject"), std::string::npos);
    EXPECT_NE(text.find("PhysicalTableScan"), std::string::npos);
}

TEST_F(PlannerExecutorTest, InitializesMatchingExecutionStateTree)
{
    const auto        scan = plan::makeNode(plan::PhysicalTableScan{.table_name = "fake.samples"});
    const auto        filter = plan::makeNode(plan::PhysicalFilter{
        .input = scan,
        .predicates = {query::Predicate{.column = "pv", .op = query::PredicateOp::EQ, .values = {std::string("A")}}}});
    const auto        project = plan::makeNode(plan::PhysicalProject{.input = filter, .columns = {"pv"}});
    query::QueryStats stats;
    const auto        state = query::executor::makeExecutionState(
        project, {.pool = arrow::default_memory_pool()}, stats);

    ASSERT_EQ(state->typeName(), "ProjectExecutionState");
    ASSERT_EQ(state->children().size(), 1U);
    EXPECT_EQ(state->children().front()->typeName(), "FilterExecutionState");
    ASSERT_EQ(state->children().front()->children().size(), 1U);
    EXPECT_EQ(state->children().front()->children().front()->typeName(), "TableScanExecutionState");
}

TEST_F(PlannerExecutorTest, ProgressTrackerSnapshotsBackendRpcAndFinalStats)
{
    FakeQueryable::execute_delay = std::chrono::milliseconds{200};
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    auto                 progress = std::make_shared<query::QueryProgressTracker>();
    auto                 future = std::async(std::launch::async,
                                             [&]
                                             {
                                 return executor.execute(
                                     planner.plan(query::parseQuery("SELECT pv FROM fake.paged")),
                                     {.pool = arrow::default_memory_pool(), .progress = progress});
                                             });

    query::QueryProgressSnapshot running;
    bool                         saw_backend_rpc = false;
    const auto                   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        running = progress->snapshot();
        if (running.phase == query::QueryProgressPhase::BackendRpc)
        {
            saw_backend_rpc = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(saw_backend_rpc);
    EXPECT_EQ(running.table_name, "fake.paged");
    EXPECT_EQ(running.rpc_calls_started, 1U);
    EXPECT_EQ(running.rpc_calls_completed, 0U);

    const auto result = future.get();
    const auto complete = progress->snapshot();
    EXPECT_EQ(result.stats.rpc_calls, 2U);
    EXPECT_EQ(result.stats.rows_from_backend, 2U);
    EXPECT_EQ(result.stats.rows_returned, 2U);
    EXPECT_EQ(complete.rpc_calls_started, 2U);
    EXPECT_EQ(complete.rpc_calls_completed, 2U);
    EXPECT_EQ(complete.rows_from_backend, 2U);
    EXPECT_EQ(complete.rows_returned, 2U);
}

TEST_F(PlannerExecutorTest, ExecutionStateFactoryMapsAllPhysicalNodeTypes)
{
    const auto                                                            scan = plan::makeNode(plan::PhysicalTableScan{.table_name = "fake.samples"});
    const std::vector<std::pair<plan::PhysicalNodePtr, std::string_view>> cases{
        {scan, "TableScanExecutionState"},
        {plan::makeNode(plan::PhysicalFilter{.input = scan}), "FilterExecutionState"},
        {plan::makeNode(plan::PhysicalProject{.input = scan}), "ProjectExecutionState"},
        {plan::makeNode(plan::PhysicalSort{.input = scan}), "SortExecutionState"},
        {plan::makeNode(plan::PhysicalLimit{.input = scan}), "LimitExecutionState"},
        {plan::makeNode(plan::PhysicalHashJoin{.left = scan, .right = scan}), "HashJoinExecutionState"},
        {plan::makeNode(plan::PhysicalNestedLoopJoin{.outer = scan, .inner = scan}), "NestedLoopJoinExecutionState"},
        {plan::makeNode(plan::PhysicalBlockNestedLoopJoin{.outer = scan, .inner = scan}), "BlockNestedLoopJoinExecutionState"},
        {plan::makeNode(plan::PhysicalShowTables{}), "ShowTablesExecutionState"},
        {plan::makeNode(plan::PhysicalDescribe{}), "DescribeExecutionState"},
        {plan::makeNode(plan::PhysicalExplain{}), "ExplainExecutionState"},
        {plan::makeNode(plan::PhysicalCreateTable{.query = scan}), "CreateTableExecutionState"},
        {plan::makeNode(plan::PhysicalDropTable{}), "DropTableExecutionState"},
    };
    for (const auto& [physical, expected] : cases)
    {
        query::QueryStats stats;
        const auto        state = query::executor::makeExecutionState(
            physical, {.pool = arrow::default_memory_pool()}, stats);
        EXPECT_EQ(state->typeName(), expected);
    }
}

TEST_F(PlannerExecutorTest, ExecutesDerivedSourceAndPlansItAsArrowIpcScan)
{
    auto                    file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                    catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner     planner(catalog);
    query::QueryExecutor    executor;

    const auto create = planner.plan(query::parseQuery("CREATE TEMP TABLE samples AS SELECT pv, value FROM fake.samples WHERE pv = 'A'"));
    const auto created = executor.execute(create, context);
    EXPECT_EQ(created.stats.rpc_calls, 1U);

    const auto stored = executor.execute(planner.plan(query::parseQuery("SELECT pv FROM samples WHERE value = 1")), context);
    EXPECT_EQ(stored.stats.rpc_calls, 0U);
    ASSERT_EQ(stored.batches.size(), 1U);
    EXPECT_EQ(stored.batches.front()->num_rows(), 1);

    const auto derived = executor.execute(planner.plan(query::parseQuery("SELECT recent.pv FROM (SELECT pv FROM samples) recent")), context);
    EXPECT_EQ(derived.stats.rpc_calls, 0U);
    ASSERT_EQ(derived.batches.size(), 1U);
    EXPECT_EQ(derived.batches.front()->num_rows(), 1);

    const auto aliasless_derived = executor.execute(planner.plan(query::parseQuery("SELECT pv FROM (SELECT pv FROM samples)")), context);
    EXPECT_EQ(aliasless_derived.stats.rpc_calls, 0U);
    ASSERT_EQ(aliasless_derived.batches.size(), 1U);
    EXPECT_EQ(aliasless_derived.batches.front()->num_rows(), 1);

    const auto empty_create = planner.plan(query::parseQuery("CREATE TEMP TABLE empty_samples AS SELECT pv, value FROM fake.samples WHERE pv = 'missing'"));
    const auto empty_created = executor.execute(empty_create, context);
    EXPECT_EQ(empty_created.stats.rpc_calls, 1U);

    const auto empty_stored = executor.execute(planner.plan(query::parseQuery("SELECT * FROM empty_samples")), context);
    EXPECT_EQ(empty_stored.stats.rpc_calls, 0U);
    ASSERT_EQ(empty_stored.batches.size(), 1U);
    ASSERT_NE(empty_stored.batches.front(), nullptr);
    EXPECT_EQ(empty_stored.batches.front()->num_rows(), 0);
    EXPECT_EQ(empty_stored.batches.front()->schema()->field(0)->name(), "pv");
    EXPECT_EQ(empty_stored.batches.front()->schema()->field(1)->name(), "value");
}

TEST(QueryPlannerExecutorTest, CreateTableDrainsEveryNativeStreamBatch)
{
    query::QueryableFactory::instance().reset();
    NativeCreateQueryable::stream_creations = 0;
    NativeCreateQueryable::next_calls = 0;
    query::QueryableFactory::instance().prepare<NativeCreateQueryable>(config::Config::configFromYamlString("{}"));
    auto                          file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                          catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    query::QueryPlanner           planner(catalog);
    query::QueryExecutor          executor;
    const query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};

    const auto created = executor.execute(
        planner.plan(query::parseQuery("CREATE TEMP TABLE native_samples AS SELECT pv, time, value FROM mldp.time_series WHERE pv = 'CREATE:PV'")), context);
    EXPECT_TRUE(created.batches.empty());
    EXPECT_EQ(NativeCreateQueryable::stream_creations, 1U);
    EXPECT_EQ(NativeCreateQueryable::next_calls, 4U);
    const auto table = catalog->find("native_samples");
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->row_count, 3);
    const auto persisted = catalog->read(*table);
    ASSERT_TRUE(persisted.ok()) << persisted.status().ToString();
    ASSERT_EQ(persisted->size(), 3U);
    for (std::size_t index = 0; index < persisted->size(); ++index)
        EXPECT_EQ(std::static_pointer_cast<arrow::Int64Array>((*persisted)[index]->GetColumnByName("value"))->Value(0), static_cast<int64_t>(index + 1));

    NativeCreateQueryable::next_calls = 0;
    const auto limited = executor.execute(
        planner.plan(query::parseQuery("CREATE TEMP TABLE native_limited AS SELECT pv, time, value FROM mldp.time_series WHERE pv = 'CREATE:PV' LIMIT 2")), context);
    EXPECT_TRUE(limited.batches.empty());
    EXPECT_EQ(NativeCreateQueryable::next_calls, 2U);
    const auto limited_table = catalog->find("native_limited");
    ASSERT_TRUE(limited_table.has_value());
    EXPECT_EQ(limited_table->row_count, 2);
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, StreamsSubqueryWindowsAcrossNormalizedRanges)
{
    query::QueryableFactory::instance().reset();
    {
        const std::lock_guard lock(SubqueryWindowQueryable::requests_mutex);
        SubqueryWindowQueryable::requests.clear();
    }
    query::QueryableFactory::instance().prepare<SubqueryWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    auto                 streamed = executor.executeStream(planner.plan(query::parseQuery(
                                                               "SELECT pv, time, value FROM mldp.time_series WHERE pv IN ('SUB:ONE', 'SUB:TWO') " "AND window IN (SELECT time, end_time FROM mldp.configuration_activation; slice 5s, series_per_shard 1) LIMIT 4")),
                                                           {.pool = arrow::default_memory_pool()});
    int64_t              rows = 0;
    while (auto batch = streamed.stream->next())
        rows += batch->num_rows();
    EXPECT_EQ(rows, 4);
    std::vector<std::vector<query::Predicate>> requests;
    {
        const std::lock_guard lock(SubqueryWindowQueryable::requests_mutex);
        requests = SubqueryWindowQueryable::requests;
    }
    ASSERT_EQ(requests.size(), 4U);
    const auto shard = [](const std::vector<query::Predicate>& predicates)
    {
        std::string pv;
        int64_t     begin = -1;
        int64_t     end = -1;
        for (const auto& predicate : predicates)
        {
            if (predicate.column == "pv")
                pv = std::get<std::string>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::GTE)
                begin = std::get<int64_t>(predicate.values.front());
            if (predicate.column == "time" && predicate.op == query::PredicateOp::LTE)
                end = std::get<int64_t>(predicate.values.front());
        }
        return std::tuple{pv, begin, end};
    };
    EXPECT_EQ(shard(requests[0]), (std::tuple{"SUB:ONE", 0LL, 5LL}));
    EXPECT_EQ(shard(requests[1]), (std::tuple{"SUB:TWO", 0LL, 5LL}));
    EXPECT_EQ(shard(requests[2]), (std::tuple{"SUB:ONE", 10LL, 15LL}));
    EXPECT_EQ(shard(requests[3]), (std::tuple{"SUB:TWO", 10LL, 15LL}));
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, StreamsPvGroupsConcurrentlyButPreservesRequestedOrder)
{
    query::QueryableFactory::instance().reset();
    {
        const std::lock_guard lock(ConcurrentWindowQueryable::mutex);
        ConcurrentWindowQueryable::active_streams = 0;
        ConcurrentWindowQueryable::peak_active_streams = 0;
        ConcurrentWindowQueryable::started_streams = 0;
    }
    query::QueryableFactory::instance().prepare<ConcurrentWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    auto                 streamed = executor.executeStream(planner.plan(query::parseQuery(
                                                               "SELECT pv, time, value FROM mldp.time_series WHERE pv IN ('PV1', 'PV2', 'PV3') " "AND window IN (0, 1; slice 2s, series_per_shard 1)")),
                                                           {.pool = arrow::default_memory_pool()});

    std::vector<std::string> pvs;
    while (const auto batch = streamed.stream->next())
    {
        const auto values = std::static_pointer_cast<arrow::StringArray>(batch->GetColumnByName("pv"));
        ASSERT_NE(values, nullptr);
        pvs.push_back(values->GetString(0));
    }

    EXPECT_EQ(pvs, (std::vector<std::string>{"PV1", "PV2", "PV3"}));
    EXPECT_EQ(ConcurrentWindowQueryable::peak_active_streams, 2U);
    EXPECT_EQ(ConcurrentWindowQueryable::started_streams, 3U);
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, ScalesProductionShapedWideWindowQueryToFourConcurrentSeriesShards)
{
    query::QueryableFactory::instance().reset();
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ScaledWindowQueryable::active_streams = 0;
        ScaledWindowQueryable::peak_active_streams = 0;
        ScaledWindowQueryable::started_streams = 0;
        ScaledWindowQueryable::metadata_execute_calls = 0;
        ScaledWindowQueryable::time_series_execute_calls = 0;
        ScaledWindowQueryable::require_parallel_wave = true;
        ScaledWindowQueryable::fail_stream_open = false;
        ScaledWindowQueryable::requests.clear();
    }
    query::QueryableFactory::instance().prepare<ScaledWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    const auto           spill = std::make_shared<query::SpillManager>(file_system, "spill");

    auto                                             streamed = executor.executeStream(planner.plan(query::parseQuery(
                                                                                           "SELECT * FROM mldp.time_series_table " "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE attributes.dname PREFIX 'USEG:UNDH') " "AND window IN (SELECT time, time + 30s FROM mldp.configuration_activation " "WHERE config_name = 'SPEAR User' LIMIT 1; slice 5s, series_per_shard 2)")),
                                                                                       {.pool = arrow::default_memory_pool(), .spill = spill});
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    while (const auto batch = streamed.stream->next())
        batches.push_back(batch);

    std::vector<std::vector<query::Predicate>> requests;
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        requests = ScaledWindowQueryable::requests;
    }
    ASSERT_EQ(requests.size(), 96U);
    EXPECT_EQ(ScaledWindowQueryable::metadata_execute_calls, 1U);
    EXPECT_EQ(ScaledWindowQueryable::peak_active_streams, 4U);
    EXPECT_EQ(ScaledWindowQueryable::started_streams, 96U);

    std::set<std::string>                           received_pvs;
    std::map<std::pair<int64_t, int64_t>, uint64_t> request_ranges;
    for (const auto& predicates : requests)
    {
        const auto pv = std::find_if(predicates.begin(), predicates.end(), [](const query::Predicate& predicate)
                                     {
                                         return predicate.column == "pv" && predicate.op == query::PredicateOp::IN;
                                     });
        ASSERT_NE(pv, predicates.end());
        ASSERT_EQ(pv->values.size(), 2U);
        for (const auto& value : pv->values)
            received_pvs.insert(std::get<std::string>(value));

        const auto begin = std::find_if(predicates.begin(), predicates.end(), [](const query::Predicate& predicate)
                                        {
                                            return predicate.column == "time" && predicate.op == query::PredicateOp::GTE;
                                        });
        const auto end = std::find_if(predicates.begin(), predicates.end(), [](const query::Predicate& predicate)
                                      {
                                          return predicate.column == "time" && predicate.op == query::PredicateOp::LTE;
                                      });
        ASSERT_NE(begin, predicates.end());
        ASSERT_NE(end, predicates.end());
        ++request_ranges[{std::get<int64_t>(begin->values.front()), std::get<int64_t>(end->values.front())}];
    }
    EXPECT_EQ(request_ranges,
              (std::map<std::pair<int64_t, int64_t>, uint64_t>{{{0, 5}, 16}, {{5, 10}, 16}, {{10, 15}, 16}, {{15, 20}, 16}, {{20, 25}, 16}, {{25, 30}, 16}}));
    EXPECT_EQ(received_pvs, std::set<std::string>(ScaledWindowQueryable::pvs().begin(), ScaledWindowQueryable::pvs().end()));
    ASSERT_FALSE(batches.empty());
    EXPECT_EQ(batches.front()->num_columns(), 33);
    std::set<int64_t> returned_timestamps;
    for (const auto& batch : batches)
    {
        const auto time = std::static_pointer_cast<arrow::TimestampArray>(batch->GetColumnByName("time"));
        ASSERT_NE(time, nullptr);
        for (int64_t row = 0; row < time->length(); ++row)
            returned_timestamps.insert(time->Value(row) / 1'000'000'000LL);
    }
    EXPECT_EQ(returned_timestamps, (std::set<int64_t>{1, 6, 11, 16, 21, 26, 30}));
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ScaledWindowQueryable::require_parallel_wave = false;
    }
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, KeepsAllMetadataSelectedWideColumnsWhenSomeHaveNoSamples)
{
    query::QueryableFactory::instance().reset();
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ScaledWindowQueryable::active_streams = 0;
        ScaledWindowQueryable::peak_active_streams = 0;
        ScaledWindowQueryable::started_streams = 0;
        ScaledWindowQueryable::metadata_execute_calls = 0;
        ScaledWindowQueryable::time_series_execute_calls = 0;
        ScaledWindowQueryable::require_parallel_wave = false;
        ScaledWindowQueryable::fail_stream_open = false;
        ScaledWindowQueryable::emitted_pv_count = 1;
        ScaledWindowQueryable::requests.clear();
    }
    query::QueryableFactory::instance().prepare<ScaledWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    const auto           spill = std::make_shared<query::SpillManager>(file_system, "spill");

    auto streamed = executor.executeStream(planner.plan(query::parseQuery(
                                             "SELECT * FROM mldp.time_series_table "
                                             "WHERE pv IN (SELECT pv FROM mldp.pv_metadata WHERE attributes.dname PREFIX 'USEG:UNDH' LIMIT 4) "
                                             "AND window IN (SELECT time, time + 5s FROM mldp.configuration_activation "
                                             "WHERE config_name = 'SPEAR User' LIMIT 1; slice 5s, series_per_shard 2)")),
                                         {.pool = arrow::default_memory_pool(), .spill = spill});
    const auto batch = streamed.stream->next();

    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->schema()->field_names(),
              (std::vector<std::string>{"time", "USEG:UNDH:1450:GapAct", "USEG:UNDH:1550:GapAct", "USEG:UNDH:1650:GapAct", "USEG:UNDH:1750:GapAct"}));
    for (const auto& pv : std::vector<std::string>{"USEG:UNDH:1550:GapAct", "USEG:UNDH:1750:GapAct"})
    {
        const auto values = batch->GetColumnByName(pv);
        ASSERT_NE(values, nullptr);
        EXPECT_EQ(values->null_count(), values->length());
    }
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        EXPECT_EQ(ScaledWindowQueryable::metadata_execute_calls, 1U);
        ASSERT_FALSE(ScaledWindowQueryable::requests.empty());
        for (const auto& predicates : ScaledWindowQueryable::requests)
        {
            const auto pv = std::find_if(predicates.begin(), predicates.end(), [](const query::Predicate& predicate)
                                         {
                                             return predicate.column == "pv" && predicate.op == query::PredicateOp::IN;
                                         });
            ASSERT_NE(pv, predicates.end());
            EXPECT_LE(pv->values.size(), 2U);
        }
        ScaledWindowQueryable::emitted_pv_count = std::numeric_limits<std::size_t>::max();
    }
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, KeepsNonPushableInSubqueriesOnTheMaterializedPath)
{
    query::QueryableFactory::instance().reset();
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ScaledWindowQueryable::active_streams = 0;
        ScaledWindowQueryable::peak_active_streams = 0;
        ScaledWindowQueryable::started_streams = 0;
        ScaledWindowQueryable::requests.clear();
        ScaledWindowQueryable::metadata_execute_calls = 0;
        ScaledWindowQueryable::time_series_execute_calls = 0;
        ScaledWindowQueryable::require_parallel_wave = false;
        ScaledWindowQueryable::fail_stream_open = false;
    }
    query::QueryableFactory::instance().prepare<ScaledWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;

    auto                                             streamed = executor.executeStream(planner.plan(query::parseQuery(
                                                                                           "SELECT pv FROM mldp.time_series WHERE pv = 'PV:01' AND value IN (SELECT value FROM mldp.pv_metadata) " "AND window IN (0, 5; slice 5s, series_per_shard 2)")),
                                                                                       {.pool = arrow::default_memory_pool()});
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    while (const auto batch = streamed.stream->next())
        batches.push_back(batch);

    ASSERT_FALSE(batches.empty());
    EXPECT_EQ(ScaledWindowQueryable::metadata_execute_calls, 1U);
    EXPECT_EQ(ScaledWindowQueryable::time_series_execute_calls, 0U);
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ASSERT_EQ(ScaledWindowQueryable::requests.size(), 1U);
        EXPECT_EQ(ScaledWindowQueryable::peak_active_streams, 1U);
        const auto pv = std::find_if(ScaledWindowQueryable::requests.front().begin(), ScaledWindowQueryable::requests.front().end(), [](const query::Predicate& predicate)
                                     {
                                         return predicate.column == "pv" && predicate.op == query::PredicateOp::IN;
                                     });
        ASSERT_NE(pv, ScaledWindowQueryable::requests.front().end());
        ASSERT_EQ(pv->values.size(), 1U);
        EXPECT_EQ(std::get<std::string>(pv->values.front()), "PV:01");
    }
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, ReportsWindowShardContextForAsynchronousStreamFailures)
{
    query::QueryableFactory::instance().reset();
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ScaledWindowQueryable::require_parallel_wave = false;
        ScaledWindowQueryable::fail_stream_open = true;
    }
    query::QueryableFactory::instance().prepare<ScaledWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;

    auto streamed = executor.executeStream(planner.plan(query::parseQuery(
                                               "SELECT pv, time, value FROM mldp.time_series WHERE pv IN ('PV:01', 'PV:02') " "AND window IN (0, 5; slice 5s, series_per_shard 2)")),
                                           {.pool = arrow::default_memory_pool()});
    try
    {
        (void)streamed.stream->next();
        FAIL() << "Expected asynchronous stream opening to fail";
    }
    catch (const std::exception& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("Window backend scan failed while opening window 1, slice 1, shard 1"), std::string::npos);
        EXPECT_NE(message.find("PVs: PV:01, PV:02"), std::string::npos);
        EXPECT_NE(message.find("Invalid argument"), std::string::npos);
    }
    {
        const std::lock_guard lock(ScaledWindowQueryable::mutex);
        ScaledWindowQueryable::fail_stream_open = false;
    }
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, AcceptsWindowShardOptionsForWideSubqueryWindows)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<SubqueryWindowQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner planner;
    EXPECT_NO_THROW((void)planner.plan(query::parseQuery(
        "SELECT * FROM mldp.time_series_table WHERE pv IN ('SUB:ONE', 'SUB:TWO') " "AND window IN (SELECT time, end_time FROM mldp.configuration_activation; slice 5s, series_per_shard 2)")));
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, ReportsSerialWideWindowShardProgress)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<WideStreamingQueryable>(config::Config::configFromYamlString("{}"));
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    auto                 progress = std::make_shared<query::QueryProgressTracker>();
    auto                 file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                 spill = std::make_shared<query::SpillManager>(file_system, "spill");

    const auto result = executor.execute(planner.plan(query::parseQuery(
                                             "SELECT * FROM mldp.time_series_table WHERE pv IN ('WIDE:ONE', 'WIDE:TWO') " "AND window IN (0, 1; slice 2s, series_per_shard 2)")),
                                         {.pool = arrow::default_memory_pool(), .spill = spill, .progress = progress});

    ASSERT_FALSE(result.batches.empty());
    const auto snapshot = progress->snapshot();
    EXPECT_EQ(snapshot.table_name, "mldp.time_series_table");
    EXPECT_EQ(snapshot.window_index, 1U);
    EXPECT_EQ(snapshot.slice_index, 1U);
    EXPECT_EQ(snapshot.series_shard_index, 1U);
    EXPECT_EQ(snapshot.series_in_shard, 2U);
    EXPECT_EQ(snapshot.parallel_shard_limit, 1U);
    EXPECT_EQ(snapshot.active_parallel_shards, 0U);
    EXPECT_EQ(snapshot.completed_shards, 1U);
    query::QueryableFactory::instance().reset();
}

TEST(QueryPlannerExecutorTest, WidePivotSortsSpilledBidiBatchesAndPreservesPvOrder)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<WideStreamingQueryable>(config::Config::configFromYamlString("{}"));
    auto                 file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                 spill = std::make_shared<query::SpillManager>(file_system, "spill");
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           physical = planner.plan(query::parseQuery(
        "SELECT * FROM mldp.time_series_table WHERE pv IN ('WIDE:ONE', 'WIDE:TWO') AND window IN (0, 1; slice 2s, series_per_shard 2)"));
    const auto           plan_text = query::plan::physicalPlanToString(physical);
    EXPECT_NE(plan_text.find("PhysicalPivot(columns=2, batch_size=4096)"), std::string::npos);
    EXPECT_NE(plan_text.find("PhysicalTableScan(table=mldp.time_series"), std::string::npos);
    const auto result = executor.execute(physical,
                                         {.pool = arrow::default_memory_pool(), .spill = spill});
    ASSERT_EQ(result.batches.size(), 1U);
    const auto& batch = result.batches.front();
    ASSERT_EQ(batch->num_rows(), 2);
    EXPECT_EQ(batch->schema()->field(1)->name(), "WIDE:ONE");
    EXPECT_EQ(batch->schema()->field(2)->name(), "WIDE:TWO");
    const auto times = std::static_pointer_cast<arrow::TimestampArray>(batch->column(0));
    const auto one = std::static_pointer_cast<arrow::Int64Array>(batch->column(1));
    const auto two = std::static_pointer_cast<arrow::Int64Array>(batch->column(2));
    EXPECT_EQ(times->Value(0), 0);
    EXPECT_EQ(times->Value(1), 1'000'000'000LL);
    EXPECT_EQ(one->Value(0), 10);
    EXPECT_EQ(two->Value(0), 20);
    EXPECT_EQ(one->Value(1), 11);
    EXPECT_EQ(two->Value(1), 21);
    EXPECT_GE(result.stats.materialized_files, 3U);
    query::QueryableFactory::instance().reset();
}

TEST_F(PlannerExecutorTest, FiltersMaterializedDenseUnionValuesNumerically)
{
    auto                     file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                     catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    const auto               value_type = arrow::dense_union({arrow::field("string", arrow::utf8()), arrow::field("double", arrow::float64())});
    auto                     string_builder = std::make_shared<arrow::StringBuilder>();
    auto                     double_builder = std::make_shared<arrow::DoubleBuilder>();
    arrow::DenseUnionBuilder value_builder(arrow::default_memory_pool(), {string_builder, double_builder}, value_type);
    ASSERT_TRUE(value_builder.Append(0).ok());
    ASSERT_TRUE(string_builder->Append("not numeric").ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(double_builder->Append(9.5).ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(double_builder->Append(10.5).ok());
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(value_builder.Finish(&value).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("value", value_type)}), 3, {value});
    ASSERT_TRUE(catalog->create("magnet_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner     planner(catalog);
    query::QueryExecutor    executor;
    const auto              result = executor.execute(planner.plan(query::parseQuery("SELECT * FROM magnet_samples WHERE value > 10.0")), context);

    EXPECT_EQ(result.stats.rpc_calls, 0U);
    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->schema()->field(0)->type()->id(), arrow::Type::DENSE_UNION);
    const auto scalar = result.batches.front()->column(0)->GetScalar(0);
    ASSERT_TRUE(scalar.ok());
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*scalar)->child_value()->ToString(), "10.5");
}

TEST_F(PlannerExecutorTest, FiltersMaterializedNativeUnionValuesByActiveTypeWithoutUnionGather)
{
    auto       file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto       catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    const auto value_type = arrow::dense_union({arrow::field("string", arrow::utf8()), arrow::field("bool", arrow::boolean()),
                                                arrow::field("int", arrow::int32()), arrow::field("float", arrow::float32()),
                                                arrow::field("double", arrow::float64()), arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO)),
                                                arrow::field("binary", arrow::binary())});
    auto       string_builder = std::make_shared<arrow::StringBuilder>();
    auto       bool_builder = std::make_shared<arrow::BooleanBuilder>();
    auto       int_builder = std::make_shared<arrow::Int32Builder>();
    auto       float_builder = std::make_shared<arrow::FloatBuilder>();
    auto       double_builder = std::make_shared<arrow::DoubleBuilder>();
    auto       timestamp_builder = std::make_shared<arrow::TimestampBuilder>(
        arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
    auto                     binary_builder = std::make_shared<arrow::BinaryBuilder>();
    arrow::DenseUnionBuilder value_builder(arrow::default_memory_pool(), {string_builder, bool_builder, int_builder, float_builder, double_builder, timestamp_builder, binary_builder}, value_type);
    ASSERT_TRUE(value_builder.Append(0).ok());
    ASSERT_TRUE(string_builder->Append("11").ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(bool_builder->Append(true).ok());
    ASSERT_TRUE(value_builder.Append(2).ok());
    ASSERT_TRUE(int_builder->Append(10).ok());
    ASSERT_TRUE(value_builder.Append(3).ok());
    ASSERT_TRUE(float_builder->Append(10.5F).ok());
    ASSERT_TRUE(value_builder.Append(4).ok());
    ASSERT_TRUE(double_builder->Append(11.0).ok());
    ASSERT_TRUE(value_builder.Append(5).ok());
    ASSERT_TRUE(timestamp_builder->Append(11).ok());
    ASSERT_TRUE(value_builder.Append(6).ok());
    ASSERT_TRUE(binary_builder->Append("11").ok());
    ASSERT_TRUE(value_builder.AppendNull().ok());
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(value_builder.Finish(&value).ok());

    arrow::StringBuilder pv_builder;
    arrow::Int64Builder  time_builder;
    for (int row = 0; row < 8; ++row)
    {
        ASSERT_TRUE(pv_builder.Append("PV:" + std::to_string(row)).ok());
        ASSERT_TRUE(time_builder.Append(100 + row).ok());
    }
    std::shared_ptr<arrow::Array> pv;
    std::shared_ptr<arrow::Array> time;
    ASSERT_TRUE(pv_builder.Finish(&pv).ok());
    ASSERT_TRUE(time_builder.Finish(&time).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("time", arrow::int64()), arrow::field("value", value_type)}), 8, {pv, time, value});
    ASSERT_TRUE(catalog->create("magnet_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner     planner(catalog);
    query::QueryExecutor    executor;
    const auto              execute = [&](const std::string_view sql)
    {
        const auto result = executor.execute(planner.plan(query::parseQuery(sql)), context);
        EXPECT_EQ(result.stats.rpc_calls, 0U);
        EXPECT_EQ(result.batches.size(), 1U);
        return result.batches.empty() ? std::shared_ptr<arrow::RecordBatch>{} : result.batches.front();
    };

    const auto greater = execute("SELECT * FROM magnet_samples WHERE value > 10.0");
    ASSERT_NE(greater, nullptr);
    ASSERT_EQ(greater->num_rows(), 2);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringScalar>(*greater->column(0)->GetScalar(0))->ToString(), "PV:3");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringScalar>(*greater->column(0)->GetScalar(1))->ToString(), "PV:4");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::Int64Scalar>(*greater->column(1)->GetScalar(0))->value, 103);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::Int64Scalar>(*greater->column(1)->GetScalar(1))->value, 104);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*greater->column(2)->GetScalar(0))->child_value()->ToString(), "10.5");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*greater->column(2)->GetScalar(1))->child_value()->ToString(), "11");

    const auto not_equal = execute("SELECT * FROM magnet_samples WHERE value != 10.0");
    const auto in = execute("SELECT * FROM magnet_samples WHERE value IN (10.0, 11.0)");
    const auto between = execute("SELECT * FROM magnet_samples WHERE value BETWEEN 10.0 AND 10.5");
    const auto equal = execute("SELECT * FROM magnet_samples WHERE value = 11.0");
    ASSERT_NE(not_equal, nullptr);
    ASSERT_NE(in, nullptr);
    ASSERT_NE(between, nullptr);
    ASSERT_NE(equal, nullptr);
    EXPECT_EQ(not_equal->num_rows(), 2);
    EXPECT_EQ(in->num_rows(), 2);
    EXPECT_EQ(between->num_rows(), 2);
    EXPECT_EQ(equal->num_rows(), 1);
}

TEST_F(PlannerExecutorTest, FormatsUtcTimestampsInIanaZonesAndFixedOffsets)
{
    auto                    file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                    catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::SECOND, "UTC"), arrow::default_memory_pool());
    ASSERT_TRUE(time.Append(1'769'538'800).ok()); // 2026-01-27T18:33:20Z
    ASSERT_TRUE(time.Append(1'753'978'800).ok()); // 2025-07-31T16:20:00Z
    ASSERT_TRUE(time.AppendNull().ok());
    std::shared_ptr<arrow::Array> timestamps;
    ASSERT_TRUE(time.Finish(&timestamps).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("time", timestamps->type())}), 3, {timestamps});
    ASSERT_TRUE(catalog->create("utc_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner     planner(catalog);
    query::QueryExecutor    executor;
    const auto              result = executor.execute(planner.plan(query::parseQuery(
                                                          "SELECT from_utc(time, 'America/Los_Angeles') AS pacific, from_utc(time, '-07:00') AS fixed FROM utc_samples")),
                                                      context);

    ASSERT_EQ(result.batches.size(), 1U);
    const auto& output = result.batches.front();
    ASSERT_EQ(output->num_rows(), 3);
    EXPECT_EQ(output->schema()->field(0)->type()->id(), arrow::Type::STRING);
    EXPECT_EQ(output->column(0)->GetScalar(0).ValueOrDie()->ToString(), "2026-01-27T10:33:20-08:00");
    EXPECT_EQ(output->column(0)->GetScalar(1).ValueOrDie()->ToString(), "2025-07-31T09:20:00-07:00");
    EXPECT_EQ(output->column(1)->GetScalar(0).ValueOrDie()->ToString(), "2026-01-27T11:33:20-07:00");
    EXPECT_FALSE(output->column(0)->GetScalar(2).ValueOrDie()->is_valid);

    EXPECT_THROW(executor.execute(planner.plan(query::parseQuery(
                                      "SELECT from_utc(time, '-7:00') FROM utc_samples")),
                                  context),
                 std::invalid_argument);
    EXPECT_THROW(executor.execute(planner.plan(query::parseQuery(
                                      "SELECT from_utc(time, 'Not/AZone') FROM utc_samples")),
                                  context),
                 std::invalid_argument);
}

TEST_F(PlannerExecutorTest, FiltersNativeTimestampAndDurationUnionValuesWithTypedLiterals)
{
    auto                     file_system = std::make_shared<arrow::fs::internal::MockFileSystem>(std::chrono::system_clock::now());
    auto                     catalog = std::make_shared<query::QueryTableCatalog>(file_system, "catalog");
    const auto               value_type = arrow::dense_union({arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::NANO)),
                                                              arrow::field("duration", arrow::duration(arrow::TimeUnit::NANO)),
                                                              arrow::field("integer", arrow::int64())});
    auto                     timestamp_builder = std::make_shared<arrow::TimestampBuilder>(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
    auto                     duration_builder = std::make_shared<arrow::DurationBuilder>(arrow::duration(arrow::TimeUnit::NANO), arrow::default_memory_pool());
    auto                     integer_builder = std::make_shared<arrow::Int64Builder>();
    arrow::DenseUnionBuilder value_builder(arrow::default_memory_pool(), {timestamp_builder, duration_builder, integer_builder}, value_type);
    ASSERT_TRUE(value_builder.Append(0).ok());
    ASSERT_TRUE(timestamp_builder->Append(10).ok());
    ASSERT_TRUE(value_builder.Append(0).ok());
    ASSERT_TRUE(timestamp_builder->Append(20).ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(duration_builder->Append(10).ok());
    ASSERT_TRUE(value_builder.Append(1).ok());
    ASSERT_TRUE(duration_builder->Append(20).ok());
    ASSERT_TRUE(value_builder.Append(2).ok());
    ASSERT_TRUE(integer_builder->Append(10).ok());
    std::shared_ptr<arrow::Array> value;
    ASSERT_TRUE(value_builder.Finish(&value).ok());

    arrow::StringBuilder pv_builder;
    for (const auto& pv : {"TS:10", "TS:20", "D:10", "D:20", "I:10"})
        ASSERT_TRUE(pv_builder.Append(pv).ok());
    std::shared_ptr<arrow::Array> pv;
    ASSERT_TRUE(pv_builder.Finish(&pv).ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("pv", arrow::utf8()), arrow::field("value", value_type)}), 5, {pv, value});
    ASSERT_TRUE(catalog->create("typed_samples", query::TableLifetime::Session, {batch}).ok());

    query::ExecutionContext context{.pool = arrow::default_memory_pool(), .table_catalog = catalog};
    query::QueryPlanner     planner(catalog);
    query::QueryExecutor    executor;
    const auto              expect = [&](const std::string_view sql, const std::vector<std::string>& expected_pvs, const std::vector<arrow::Type::type>& active_types)
    {
        const auto result = executor.execute(planner.plan(query::parseQuery(sql)), context);
        EXPECT_EQ(result.stats.rpc_calls, 0U);
        ASSERT_EQ(result.batches.size(), 1U);
        const auto& selected = result.batches.front();
        ASSERT_EQ(selected->num_rows(), static_cast<int64_t>(expected_pvs.size()));
        ASSERT_EQ(active_types.size(), expected_pvs.size());
        if (expected_pvs.empty())
            return;
        for (int64_t row = 0; row < selected->num_rows(); ++row)
        {
            const auto pv_scalar = selected->column(0)->GetScalar(row);
            const auto value_scalar = selected->column(1)->GetScalar(row);
            ASSERT_TRUE(pv_scalar.ok());
            ASSERT_TRUE(value_scalar.ok());
            EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringScalar>(*pv_scalar)->ToString(), expected_pvs[static_cast<std::size_t>(row)]);
            EXPECT_EQ(std::dynamic_pointer_cast<arrow::UnionScalar>(*value_scalar)->child_value()->type->id(), active_types[static_cast<std::size_t>(row)]);
        }
    };

    expect("SELECT * FROM typed_samples WHERE value = timestamp_ns(10)", {"TS:10"}, {arrow::Type::TIMESTAMP});
    expect("SELECT * FROM typed_samples WHERE value != timestamp_ns(10)", {"TS:20"}, {arrow::Type::TIMESTAMP});
    expect("SELECT * FROM typed_samples WHERE value IN (duration_ns(20), timestamp_ns(10))", {"TS:10", "D:20"}, {arrow::Type::TIMESTAMP, arrow::Type::DURATION});
    expect("SELECT * FROM typed_samples WHERE value BETWEEN timestamp_ns(10) AND timestamp_ns(20)", {"TS:10", "TS:20"}, {arrow::Type::TIMESTAMP, arrow::Type::TIMESTAMP});
    expect("SELECT * FROM typed_samples WHERE value > duration_ns(10)", {"D:20"}, {arrow::Type::DURATION});
    expect("SELECT * FROM typed_samples WHERE value BETWEEN duration_ns(10) AND timestamp_ns(20)", {}, {});
}

TEST_F(PlannerExecutorTest, PushesBackendPredicateAndPrunesProjectionColumns)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A'"));
    const auto*         scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates[0].column, "pv");
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv"}));
    EXPECT_EQ(query::plan::physicalPlanToString(plan).find("PhysicalFilter"), std::string::npos);
}

TEST_F(PlannerExecutorTest, FoldsToUtcPredicateToEpochSecondsBeforePushdown)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.samples WHERE pv = 'A' AND time >= to_utc('1970-01-01T00:00:10Z')"));
    const auto*         scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 2U);

    const auto time = std::find_if(scan->pushable_predicates.begin(), scan->pushable_predicates.end(), [](const query::Predicate& predicate)
                                   {
                                       return predicate.column == "time";
                                   });
    ASSERT_NE(time, scan->pushable_predicates.end());
    ASSERT_EQ(time->op, query::PredicateOp::GTE);
    ASSERT_EQ(time->values.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<int64_t>(time->values.front()));
    EXPECT_EQ(std::get<int64_t>(time->values.front()), 10);
}

TEST(ActivationTimestampPredicateTest, RetainsTimestampPredicatesForExactLocalFiltering)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<ActivationTimestampQueryable>(config::Config::configFromYamlString("{}"));
    ActivationTimestampQueryable::received_predicates.clear();

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT time, end_time FROM mldp.configuration_activation " "WHERE time >= 7 AND end_time <= 20")),
        {.pool = arrow::default_memory_pool()});

    ASSERT_EQ(ActivationTimestampQueryable::received_predicates.size(), 2U);
    EXPECT_EQ(ActivationTimestampQueryable::received_predicates[0].column, "time");
    EXPECT_EQ(ActivationTimestampQueryable::received_predicates[1].column, "end_time");
    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_EQ(result.batches.front()->num_rows(), 1);
    query::QueryableFactory::instance().reset();
}

TEST(ActivationTimestampPredicateTest, FiltersOpenActivationsWithIsNull)
{
    query::QueryableFactory::instance().reset();
    query::QueryableFactory::instance().prepare<ActivationTimestampQueryable>(config::Config::configFromYamlString("{}"));

    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(
        planner.plan(query::parseQuery(
            "SELECT time FROM mldp.configuration_activation WHERE end_time IS NULL")),
        {.pool = arrow::default_memory_pool()});

    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_EQ(result.batches.front()->num_rows(), 1);
    query::QueryableFactory::instance().reset();
}

TEST_F(PlannerExecutorTest, RetainsFilterableOnlyPredicateForLocalExecution)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' AND value = 1"));
    const auto*         project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1);
    EXPECT_EQ(filter->predicates[0].column, "value");
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates[0].column, "pv");
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv", "value"}));
}

TEST_F(PlannerExecutorTest, ExecutesGenericPushableInSubqueryBeforeDependentScan)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.samples WHERE pv IN (SELECT pv FROM fake.meta WHERE pv = 'A')"));
    const auto*          scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->in_subqueries.size(), 1U);
    EXPECT_TRUE(scan->in_subqueries.front().pushable);
    EXPECT_EQ(scan->in_subqueries.front().predicate.column, "pv");

    const auto result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1U);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
    EXPECT_EQ(result.stats.rpc_calls, 2U);
}

TEST_F(PlannerExecutorTest, AppliesGenericLocalInSubqueryAfterFetch)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.samples WHERE pv = 'A' AND value IN (SELECT value FROM fake.samples WHERE pv = 'A')"));
    const auto*          scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->in_subqueries.size(), 1U);
    EXPECT_FALSE(scan->in_subqueries.front().pushable);

    const auto result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.stats.rpc_calls, 2U);
}

TEST_F(PlannerExecutorTest, RetainsPushedMetadataPredicatesForLocalVerification)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND tag IN ('sample', 'magnet')"));
    const auto*         project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1);
    EXPECT_EQ(filter->predicates.front().column, "tag");
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 2);
    EXPECT_TRUE(scan->projection_hint.contains("tags"));
}

TEST_F(PlannerExecutorTest, LikeIsCaseInsensitiveAndRemainsLocal)
{
    query::QueryPlanner planner;
    const auto          plan = planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE '%L_CE'"));
    const auto*         project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1);
    EXPECT_EQ(filter->predicates.front().op, query::PredicateOp::LIKE);
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1);
    EXPECT_EQ(scan->pushable_predicates.front().column, "pv");

    query::QueryExecutor executor;
    const auto           result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
}

TEST_F(PlannerExecutorTest, LikeSupportsStarAndEscapedWildcards)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;

    const auto star_result = executor.execute(
        planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE 'A*E'")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(star_result.batches.front()->num_rows(), 1);
    EXPECT_EQ(star_result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");

    const auto escaped_result = executor.execute(
        planner.plan(query::parseQuery(R"(SELECT pv FROM fake.meta WHERE pv = 'A' AND owner LIKE 'alice\%')")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(escaped_result.batches.front()->num_rows(), 0);
}

TEST_F(PlannerExecutorTest, ReportsBinderAndTypeFailuresWithTypedErrors)
{
    query::QueryPlanner planner;
    for (const std::string_view sql : {
             "SELECT pv FROM fake.unknown WHERE pv = 'A'",
             "SELECT unknown FROM fake.samples WHERE pv = 'A'",
             "SELECT pv FROM fake.samples WHERE time = 'not-a-time'",
             "SELECT pv FROM fake.samples WHERE pv >= 'A'",
             "SELECT value FROM fake.samples"})
    {
        try
        {
            (void)planner.plan(query::parseQuery(sql));
            FAIL() << "Expected planning failure for " << sql;
        }
        catch (const query::plan::PlannerException& error)
        {
            EXPECT_TRUE(std::holds_alternative<query::plan::BindError>(error.error()) ||
                        std::holds_alternative<query::plan::TypeError>(error.error()) ||
                        std::holds_alternative<query::plan::PlanError>(error.error()));
        }
    }
}

TEST_F(PlannerExecutorTest, ExplainShortCircuitsToPlanRow)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("EXPLAIN SELECT pv FROM fake.samples WHERE pv = 'A'");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches[0]->num_rows(), 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "plan");
}

TEST_F(PlannerExecutorTest, ExecutesFilterProjectAndLimit)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("SELECT pv FROM fake.samples WHERE pv = 'A' LIMIT 1");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.stats.rows_returned, 1);
    EXPECT_EQ(result.batches[0]->schema()->num_fields(), 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "pv");
}

TEST_F(PlannerExecutorTest, ProjectsComputedIntegerExpressionsWithGeneratedAndExplicitNames)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(
        planner.plan(query::parseQuery("SELECT value + 1, value * 2 AS doubled FROM fake.samples WHERE pv = 'A'")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1U);
    const auto& batch = result.batches.front();
    ASSERT_EQ(batch->num_rows(), 1);
    EXPECT_EQ(batch->schema()->field(0)->name(), "value_1");
    EXPECT_EQ(batch->schema()->field(1)->name(), "doubled");
    EXPECT_EQ(std::static_pointer_cast<arrow::Int64Array>(batch->column(0))->Value(0), 2);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int64Array>(batch->column(1))->Value(0), 2);
}

TEST_F(PlannerExecutorTest, AccumulatesBackendPagesAndTracksEveryRpc)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery("SELECT pv FROM fake.paged"));
    const auto           result = executor.execute(plan, query::ExecutionContext{.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 2);
    EXPECT_EQ(result.stats.rpc_calls, 2);
    EXPECT_EQ(result.stats.rows_from_backend, 2);
    EXPECT_EQ(result.stats.rows_returned, 2);
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringArray>(result.batches[0]->column(0))->GetString(0), "A");
    EXPECT_EQ(std::dynamic_pointer_cast<arrow::StringArray>(result.batches[1]->column(0))->GetString(0), "B");
}

TEST_F(PlannerExecutorTest, LegacyQueryableStreamYieldsEveryContinuationPage)
{
    auto       queryable = query::QueryableFactory::instance().createByTable("fake.paged");
    auto       stream = queryable->executeStream("fake.paged", {}, {"pv"}, {.pool = arrow::default_memory_pool()});
    const auto first = stream->next();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(std::static_pointer_cast<arrow::StringArray>(first->column(0))->GetString(0), "A");
    const auto second = stream->next();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(std::static_pointer_cast<arrow::StringArray>(second->column(0))->GetString(0), "B");
    EXPECT_EQ(stream->next(), nullptr);
}

TEST_F(PlannerExecutorTest, ShowTablesReturnsRegisteredTables)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
    };

    const auto statement = query::parseQuery("SHOW TABLES");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_GE(result.batches[0]->num_rows(), 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "table_name");
    EXPECT_EQ(result.batches[0]->schema()->field(1)->name(), "type");
    EXPECT_EQ(result.batches[0]->schema()->field(2)->name(), "location");
    EXPECT_EQ(result.batches[0]->column(1)->GetScalar(0).ValueOrDie()->ToString(), "virtual");
    EXPECT_EQ(result.batches[0]->column(2)->GetScalar(0).ValueOrDie()->ToString(), "");
}

TEST_F(PlannerExecutorTest, ShowFunctionsAndOperatorsExposeSortedCallableCatalogs)
{
    query::QueryPlanner           planner;
    query::QueryExecutor          executor;
    const query::ExecutionContext context{.pool = arrow::default_memory_pool()};

    const auto functions = executor.execute(planner.plan(query::parseQuery("SHOW FUNCTIONS")), context);
    ASSERT_EQ(functions.batches.size(), 1U);
    const auto& function_batch = functions.batches.front();
    EXPECT_EQ(function_batch->schema()->field(0)->name(), "name");
    EXPECT_EQ(function_batch->schema()->field(1)->name(), "arguments");
    EXPECT_EQ(function_batch->schema()->field(2)->name(), "returns");
    ASSERT_EQ(function_batch->num_rows(), 3);
    EXPECT_EQ(function_batch->column(0)->GetScalar(0).ValueOrDie()->ToString(), "from_utc");
    EXPECT_EQ(function_batch->column(1)->GetScalar(0).ValueOrDie()->ToString(), "(timestamp, string)");
    EXPECT_EQ(function_batch->column(0)->GetScalar(1).ValueOrDie()->ToString(), "to_utc");
    EXPECT_EQ(function_batch->column(1)->GetScalar(1).ValueOrDie()->ToString(), "(string)");
    EXPECT_EQ(function_batch->column(1)->GetScalar(2).ValueOrDie()->ToString(), "(string, string)");

    const auto operators = executor.execute(planner.plan(query::parseQuery("SHOW OPERATORS")), context);
    ASSERT_EQ(operators.batches.size(), 1U);
    const auto& operator_batch = operators.batches.front();
    EXPECT_EQ(operator_batch->schema()->field(0)->name(), "symbol");
    EXPECT_EQ(operator_batch->schema()->field(1)->name(), "arity");
    EXPECT_EQ(operator_batch->schema()->field(2)->name(), "arguments");
    ASSERT_GT(operator_batch->num_rows(), 1);
    for (int64_t row = 1; row < operator_batch->num_rows(); ++row)
    {
        EXPECT_LE(operator_batch->column(0)->GetScalar(row - 1).ValueOrDie()->ToString(), operator_batch->column(0)->GetScalar(row).ValueOrDie()->ToString());
    }
}

TEST_F(PlannerExecutorTest, DescribeUsesReadableTypesAndOperators)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           result = executor.execute(
        planner.plan(query::parseQuery("DESCRIBE fake.samples")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    const auto& batch = result.batches.front();
    EXPECT_EQ(batch->column(1)->GetScalar(0).ValueOrDie()->ToString(), "string");
    EXPECT_EQ(batch->column(4)->GetScalar(0).ValueOrDie()->ToString(), "=,IN");
    EXPECT_EQ(batch->column(1)->GetScalar(1).ValueOrDie()->ToString(), "timestamp");
    EXPECT_EQ(batch->column(4)->GetScalar(1).ValueOrDie()->ToString(), "<=,>=");
}

TEST_F(PlannerExecutorTest, ExecutesInnerJoinWithQualifiedColumns)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .join_batch_size = 100,
    };

    const auto statement = query::parseQuery(
        "SELECT s.pv, m.owner FROM fake.samples s INNER JOIN fake.meta m ON s.pv = m.pv WHERE s.pv IN ('A', 'B')");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.stats.rows_returned, 1);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "s.pv");
    EXPECT_EQ(result.batches[0]->schema()->field(1)->name(), "m.owner");
}

TEST_F(PlannerExecutorTest, OrdersByAnUnselectedColumnBeforeApplyingLimit)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery("SELECT pv FROM fake.samples WHERE pv IN ('A', 'B') ORDER BY time DESC LIMIT 1"));
    const auto*          scan = findScan(plan);
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->projection_hint, std::set<std::string>({"pv", "time"}));

    const auto result = executor.execute(
        plan,
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "B");
}

TEST_F(PlannerExecutorTest, SelectsFiltersAndOrdersDynamicAttributeColumns)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;

    const auto filtered = executor.execute(
        planner.plan(query::parseQuery("SELECT pv, attributes.device_group FROM fake.meta WHERE pv IN ('A', 'C') AND attributes.device_group = 'RF'")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(filtered.batches.size(), 1);
    ASSERT_EQ(filtered.batches.front()->num_rows(), 1);
    EXPECT_EQ(filtered.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
    EXPECT_EQ(filtered.batches.front()->column(1)->GetScalar(0).ValueOrDie()->ToString(), "RF");

    const auto ordered = executor.execute(
        planner.plan(query::parseQuery("SELECT pv FROM fake.meta WHERE pv IN ('A', 'C') ORDER BY attributes.device_group DESC LIMIT 1")),
        {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(ordered.batches.size(), 1);
    ASSERT_EQ(ordered.batches.front()->num_rows(), 1);
    EXPECT_EQ(ordered.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "A");
}

TEST_F(PlannerExecutorTest, FiltersDynamicAttributePrefixLocallyAfterMaterialization)
{
    query::QueryPlanner  planner;
    query::QueryExecutor executor;
    const auto           plan = planner.plan(query::parseQuery(
        "SELECT pv FROM fake.meta WHERE pv IN ('A', 'C') AND attributes.device_group PREFIX 'M'"));

    const auto* project = std::get_if<plan::PhysicalProject>(&plan->value);
    ASSERT_NE(project, nullptr);
    const auto* filter = std::get_if<plan::PhysicalFilter>(&project->input->value);
    ASSERT_NE(filter, nullptr);
    ASSERT_EQ(filter->predicates.size(), 1U);
    EXPECT_EQ(filter->predicates.front().column, "attributes.device_group");
    EXPECT_EQ(filter->predicates.front().op, query::PredicateOp::PREFIX);
    const auto* scan = findScan(filter->input);
    ASSERT_NE(scan, nullptr);
    ASSERT_EQ(scan->pushable_predicates.size(), 1U);
    EXPECT_EQ(scan->pushable_predicates.front().column, "pv");
    EXPECT_EQ(scan->pushable_predicates.front().op, query::PredicateOp::IN);
    EXPECT_TRUE(scan->projection_hint.contains("attributes.device_group"));

    const auto result = executor.execute(plan, {.pool = arrow::default_memory_pool()});
    ASSERT_EQ(result.batches.size(), 1U);
    ASSERT_EQ(result.batches.front()->num_rows(), 1);
    EXPECT_EQ(result.batches.front()->column(0)->GetScalar(0).ValueOrDie()->ToString(), "C");
}

TEST_F(PlannerExecutorTest, ExecutesLeftJoinWithNullRightSide)
{
    query::QueryPlanner     planner;
    query::QueryExecutor    executor;
    query::ExecutionContext context{
        .pool = arrow::default_memory_pool(),
        .join_batch_size = 100,
    };

    const auto statement = query::parseQuery(
        "SELECT * FROM fake.samples s LEFT JOIN fake.meta m ON s.pv = m.pv WHERE s.pv IN ('A', 'B')");
    const auto plan = planner.plan(statement);
    const auto result = executor.execute(plan, context);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.stats.rows_returned, 2);
    EXPECT_EQ(result.batches[0]->schema()->field(0)->name(), "s.pv");
    EXPECT_EQ(result.batches[0]->schema()->field(3)->name(), "m.pv");
    EXPECT_EQ(result.batches[0]->schema()->field(4)->name(), "m.owner");
}

TEST_F(PlannerExecutorTest, RejectsAmbiguousUnqualifiedColumnAcrossJoin)
{
    query::QueryPlanner planner;
    const auto          statement = query::parseQuery("SELECT pv FROM fake.samples s JOIN fake.meta m ON s.pv = m.pv");
    EXPECT_THROW((void)planner.plan(statement), query::plan::PlannerException);
}

} // namespace
