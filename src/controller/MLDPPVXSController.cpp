//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "util/log/Logger.h"
#include <controller/MLDPPVXSController.h>
#include <processor/ChannelProcessorFactory.h>
#include <future>
#include <thread>
#include <memory>
#include <query/QueryableFactory.h>
#include <query/impl/mldp/MLDPAnnotationQueryClient.h>
#include <query/impl/mldp/MLDPQueryClient.h>
#include <reader/ReaderFactory.h>
#include <util/StringFormat.h>
#include <writer/WriterFactory.h>

#include <chrono>
#include <functional>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

using namespace mldp_pvxs_driver::metrics;
using namespace mldp_pvxs_driver::controller;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::config;
using namespace mldp_pvxs_driver::reader;
using namespace mldp_pvxs_driver::util::log;
using namespace mldp_pvxs_driver::writer;

namespace {
// Creates a dedicated logger for controller lifecycle and bus operations.
std::shared_ptr<mldp_pvxs_driver::util::log::ILogger> makeControllerLogger(const std::string& name)
{
    return mldp_pvxs_driver::util::log::newLogger("controller." + name);
}

static void prepareQueryables(const MLDPPVXSControllerConfig&                     cfg,
                              std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> metrics)
{
    using namespace mldp_pvxs_driver::query;
    using namespace mldp_pvxs_driver::query::impl::mldp;

    using PrepFn = std::function<void(const mldp_pvxs_driver::config::Config&,
                                      std::shared_ptr<mldp_pvxs_driver::metrics::Metrics>)>;
    static const std::unordered_map<std::string, PrepFn> kDispatch = {
        {"mldp",
         [](const mldp_pvxs_driver::config::Config&             c,
            std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> m)
         {
             QueryableFactory::instance().prepare<MLDPQueryClient>(c, std::move(m));
         }},
        {"mldp-pv-metadata",
         [](const mldp_pvxs_driver::config::Config&             c,
            std::shared_ptr<mldp_pvxs_driver::metrics::Metrics> m)
         {
             QueryableFactory::instance().prepare<MLDPAnnotationQueryClient>(c, std::move(m));
         }},
    };
    for (const auto& entry : cfg.queryableEntries())
    {
        auto it = kDispatch.find(entry.type);
        if (it == kDispatch.end())
        {
            throw std::runtime_error("Unknown queryable type: " + entry.type);
        }
        it->second(entry.cfg, metrics);
    }
}

void validateProcessorOutputSourceCollisions(
    const std::unordered_set<std::string>& reader_names,
    const std::vector<mldp_pvxs_driver::processor::IChannelProcessorUPtr>& processors)
{
    for (const auto& processor : processors)
    {
        for (const auto& output_source : processor->outputSourceNames())
        {
            if (reader_names.find(output_source) != reader_names.end())
            {
                throw std::runtime_error("Controller: processor output source '" + output_source +
                                         "' collides with reader name");
            }
        }
    }
}

std::string describeCycle(const std::vector<std::string>& stack, const std::string& name)
{
    auto it = std::find(stack.begin(), stack.end(), name);
    if (it == stack.end())
    {
        return name;
    }

    std::ostringstream cycle;
    for (auto current = it; current != stack.end(); ++current)
    {
        if (current != it)
        {
            cycle << " -> ";
        }
        cycle << *current;
    }
    cycle << " -> " << name;
    return cycle.str();
}

void validateProcessorGraphAcyclic(
    const std::vector<mldp_pvxs_driver::processor::IChannelProcessorUPtr>& processors)
{
    std::unordered_map<std::string, std::vector<std::string>> graph;
    std::unordered_map<std::string, std::unordered_set<std::string>> outputs_by_processor;

    for (const auto& processor : processors)
    {
        const auto output_sources = processor->outputSourceNames();
        outputs_by_processor.emplace(processor->name(),
                                     std::unordered_set<std::string>(output_sources.begin(),
                                                                     output_sources.end()));
        graph.emplace(processor->name(), std::vector<std::string>{});
    }

    for (const auto& producer : processors)
    {
        const auto& producer_outputs = outputs_by_processor.at(producer->name());
        for (const auto& consumer : processors)
        {
            if (producer->name() == consumer->name())
            {
                continue;
            }

            const auto& inputs = consumer->inputSourceNames();
            const auto depends_on_producer = std::any_of(
                inputs.begin(), inputs.end(),
                [&](const std::string& input) { return producer_outputs.find(input) != producer_outputs.end(); });
            if (depends_on_producer)
            {
                graph[producer->name()].push_back(consumer->name());
            }
        }
    }

    enum class VisitState
    {
        NotVisited,
        Visiting,
        Visited,
    };

    std::unordered_map<std::string, VisitState> state;
    std::vector<std::string> stack;

    std::function<void(const std::string&)> dfs = [&](const std::string& node)
    {
        state[node] = VisitState::Visiting;
        stack.push_back(node);

        for (const auto& next : graph[node])
        {
            if (state[next] == VisitState::Visiting)
            {
                throw std::runtime_error("Controller: processor cycle detected: " + describeCycle(stack, next));
            }
            if (state[next] == VisitState::Visited)
            {
                continue;
            }
            dfs(next);
        }

        stack.pop_back();
        state[node] = VisitState::Visited;
    };

    for (const auto& processor : processors)
    {
        if (state[processor->name()] == VisitState::NotVisited)
        {
            dfs(processor->name());
        }
    }
}
} // namespace

std::shared_ptr<MLDPPVXSController> MLDPPVXSController::create(const config::Config& config)
{
    return std::shared_ptr<MLDPPVXSController>(new MLDPPVXSController(config));
}

MLDPPVXSController::MLDPPVXSController(const config::Config& config)
    : config_(config)
    , logger_(makeControllerLogger(config_.name()))
    , thread_pool_(std::make_shared<BS::light_thread_pool>(1)) // resized in start()
    , metrics_(std::make_shared<metrics::Metrics>(*config_.metricsConfig(), config_.name()))
    , running_(false)
{
}

MLDPPVXSController::~MLDPPVXSController()
{
    if (running_.load())
    {
        stop();
    }
    if (consumer_thread_.joinable())
        consumer_thread_.join();
    tracef(*logger_, "~MLDPPVXSController [tid={}]: resetting thread_pool", std::this_thread::get_id());
    processor_pools_.clear();
    thread_pool_.reset();
    tracef(*logger_, "~MLDPPVXSController [tid={}]: resetting metrics", std::this_thread::get_id());
    metrics_.reset();
    tracef(*logger_, "~MLDPPVXSController [tid={}]: done", std::this_thread::get_id());
}

void MLDPPVXSController::start()
{
    if (running_.load())
    {
        warnf(*logger_, "Controller is already started");
        return;
    }

    // Validate minimal requirements before allocating any resources.
    if (config_.writerEntries().empty())
    {
        throw std::runtime_error("Controller: no writers are configured; add at least one writer instance under 'writer:'");
    }
    if (config_.readerEntries().empty())
    {
        throw std::runtime_error("Controller: no readers are configured; add at least one reader instance under 'reader:'");
    }

    running_.store(true);
    infof(*logger_, "Controller is starting");
    infof(*logger_, "Controller config: name='{}' queue_capacity={} writers={} readers={}",
          config_.name(),
          config_.queueCapacity(),
          config_.writerEntries().size(),
          config_.readerEntries().size());

    // Register queryable factories before any worker thread runs.
    prepareQueryables(config_, metrics_);

    // Resize the fan-out thread pool to match the number of writer instances.
    const std::size_t numWriters = config_.writerEntries().size();
    thread_pool_ = std::make_shared<BS::light_thread_pool>(
        std::max(numWriters, std::size_t{1}),
        [](std::size_t i)
        {
            BS::this_thread::set_os_thread_name("ctrl-pool-" + std::to_string(i));
        });

    // -- Build writers via factory from configured entries --
    for (const auto& [type, writerNode] : config_.writerEntries())
    {
        auto w = WriterFactory::create(type, writerNode, metrics_);
        w->start();
        writers_.push_back(std::move(w));
    }

    // Each processor gets its own dedicated 1-thread pool so algorithms run isolated
    // and independently. Separate from thread_pool_ (writer fan-out) to prevent
    // deadlock: processor tasks call bus_->push() which submits to thread_pool_ and
    // blocks waiting for futures — sharing a pool would deadlock.
    std::size_t proc_idx = 0;
    for (const auto& [type, processorNode] : config_.processorEntries())
    {
        auto pool = std::make_shared<BS::light_thread_pool>(
            1,
            [proc_idx](std::size_t)
            {
                BS::this_thread::set_os_thread_name("proc-" + std::to_string(proc_idx));
            });
        processor_pools_.push_back(pool);
        ++proc_idx;

        auto batch = processor::ChannelProcessorFactory::create(type, processorNode, shared_from_this(), metrics_, std::move(pool));
        processors_.insert(processors_.end(),
                           std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
    }
    for (auto& processor : processors_)
    {
        processor->start();
    }

    {
        std::unordered_set<std::string> reader_name_set;
        for (const auto& entry : config_.readerEntries())
            reader_name_set.insert(entry.second.get("name", ""));
        validateProcessorOutputSourceCollisions(reader_name_set, processors_);
    }
    validateProcessorGraphAcyclic(processors_);

    // -- Readers --
    infof(*logger_, "Starting readers");
    for (const auto& entry : config_.readerEntries())
    {
        const auto& type = entry.first;
        const auto& readerConfig = entry.second;
        auto        reader = ReaderFactory::create(type, shared_from_this(), readerConfig, metrics_);
        reader->setLifecycleObserver(shared_from_this());
        readers_.push_back(std::move(reader));
    }

    // -- Build route table from config --
    {
        std::unordered_set<std::string> known_writers;
        for (const auto& w : writers_)
            known_writers.insert(w->name());
        for (const auto& p : processors_)
            known_writers.insert(p->name());

        std::unordered_set<std::string> known_readers;
        for (const auto& r : readers_)
            known_readers.insert(r->name());
        for (const auto& p : processors_)
            known_readers.insert(p->outputReaderName());

        route_table_ = RouteTable::build(config_.routeEntries(), known_readers, known_writers);

        // Warn about orphan readers/writers
        for (const auto& name : route_table_.orphanReaders(known_readers))
            warnf(*logger_, "Reader '{}' not mentioned in any route — will not feed any writer", name);
        for (const auto& name : route_table_.orphanWriters(known_writers))
            warnf(*logger_, "Writer '{}' not mentioned in any route — will receive no data", name);
    }

    // Start consumer thread BEFORE readers so that onReaderCompleted → stop()
    // can always find a joinable consumer_thread_.
    consumer_thread_ = std::thread([this] { consumerLoop(); });

    infof(*logger_, "Controller started");
}

void MLDPPVXSController::stop()
{
    if (stopping_.exchange(true))
    {
        while (!stopped_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return;
    }

    running_.store(false);
    infof(*logger_, "Controller is stopping");

    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        infof(*logger_, "Removing {} reader(s)", readers_.size());
        readers_.clear();
    }

    // Wake blocked pushers and consumer thread, then join consumer.
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        debugf(*logger_, "stop() — queue depth before drain: {}", queue_.size());
    }
    queue_not_full_.notify_all();
    queue_not_empty_.notify_all();
    if (consumer_thread_.joinable())
        consumer_thread_.join();

    infof(*logger_, "Consumer thread joined — queue drained");

    for (auto& processor : processors_)
    {
        debugf(*logger_, "Stopping processor '{}'", processor->name());
        processor->stop();
    }
    processors_.clear();

    for (auto& w : writers_)
    {
        infof(*logger_, "Stopping writer '{}' — waiting for internal queue drain...", w->name());
        w->stop();
        infof(*logger_, "Writer '{}' stopped", w->name());
    }
    writers_.clear();

    infof(*logger_, "Controller stopped");
    stopped_.store(true);
}

void MLDPPVXSController::forceStop()
{
    force_quit_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        infof(*logger_, "forceStop() — discarding {} queued batch(es)", queue_.size());
        queue_.clear();
    }
    queue_not_full_.notify_all();
    queue_not_empty_.notify_all();
    for (auto& w : writers_)
        w->forceStop();
}

void MLDPPVXSController::onReaderCompleted(const std::string& reader_name)
{
    if (!running_.load())
    {
        return;
    }

    std::thread([self = shared_from_this(), reader_name]
    {
        if (!self->removeCompletedReader(reader_name))
        {
            return;
        }

        if (self->running_.load())
        {
            self->stop();
        }
    }).detach();
}

bool MLDPPVXSController::removeCompletedReader(const std::string& reader_name)
{
    std::lock_guard<std::mutex> lock(readers_mutex_);
    auto it = std::find_if(readers_.begin(), readers_.end(),
                           [&](const auto& reader)
                           {
                               return reader != nullptr && reader->name() == reader_name;
                           });
    if (it == readers_.end())
    {
        debugf(*logger_, "Ignoring completion for unknown reader '{}'", reader_name);
        return false;
    }

    infof(*logger_, "Reader '{}' completed; removing it from the active lifecycle set", reader_name);
    readers_.erase(it);
    if (!readers_.empty())
    {
        return false;
    }

    infof(*logger_, "All readers completed; triggering controller shutdown");
    return true;
}

bool MLDPPVXSController::push(EventBatch batch_values)
{
    if (!running_.load())
    {
        debugf(*logger_, "push() rejected — controller not running");
        return false;
    }

    const std::string rootSource = getRootSourceName(batch_values);

    if (rootSource.empty())
    {
        warnf(*logger_, "Received batch with empty root source, skipping push.");
        return false;
    }

    if (isTimeSeries(batch_values))
    {
        const auto& ts = asTimeSeries(batch_values);
        if (ts.frames.empty() && !ts.end_of_batch_group)
        {
            warnf(*logger_, "Received empty batch for root source {}, skipping push.", rootSource);
            return false;
        }
    }

    if (!route_table_.isAllToAll() && batch_values.reader_name.empty())
    {
        warnf(*logger_, "Batch from source '{}' has empty reader_name — routing may drop it", rootSource);
    }

    // Processors are called inline — they are noexcept, fast (ingest + snapshot),
    // and their fireCompute() calls bus_->push() recursively on the calling thread.
    // Submitting them to the thread pool would cause deadlock when a pool thread
    // re-enters push() and tries to wait on another pool task.
    //
    // Skip processor fan-out for batches emitted by processors themselves
    // (reader_name matches a processor's outputReaderName) to prevent infinite recursion.
    const bool from_processor = std::any_of(
        processors_.begin(), processors_.end(),
        [&](const auto& p) { return p->outputReaderName() == batch_values.reader_name; });

    if (!from_processor)
    {
        for (std::size_t i = 0; i < processors_.size(); ++i)
        {
            if (!route_table_.accepts(processors_[i]->name(), batch_values.reader_name))
                continue;

            if (!route_table_.acceptsSource(processors_[i]->name(), rootSource))
                continue;

            if (!processors_[i]->acceptsPayload(batch_values.payload))
                continue;

            EventBatch batchCopy = batch_values;
            processors_[i]->push(std::move(batchCopy));
        }
    }

    // Enqueue with blocking backpressure — never drops while running_.
    {
        std::unique_lock<std::mutex> lk(queue_mutex_);
        const std::size_t depthBefore = queue_.size();
        if (depthBefore >= config_.queueCapacity())
        {
            debugf(*logger_, "push() blocking — queue full ({}/{}) for source '{}'",
                   depthBefore, config_.queueCapacity(), rootSource);
        }
        queue_not_full_.wait(lk, [this] {
            return queue_.size() < config_.queueCapacity() || !running_.load();
        });
        if (!running_.load())
        {
            debugf(*logger_, "push() unblocked by stop — not running, source '{}'", rootSource);
            return false;
        }

        queue_.push_back(std::move(batch_values));
        const std::size_t depthAfter = queue_.size();
        if (metrics_)
            metrics_->setControllerQueueDepth(static_cast<double>(depthAfter));
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> plk(push_log_mutex_);
            if (now - last_push_log_time_ >= std::chrono::seconds(10))
            {
                last_push_log_time_ = now;
                debugf(*logger_, "push() accepted — queue depth {}/{} source '{}'",
                       depthAfter, config_.queueCapacity(), rootSource);
            }
        }
    }
    queue_not_empty_.notify_one();
    return true;
}

Metrics& MLDPPVXSController::metrics() const
{
    if (!metrics_)
    {
        throw std::runtime_error("Metrics not configured for controller");
    }
    return *metrics_;
}

void MLDPPVXSController::consumerLoop()
{
    infof(*logger_, "Consumer thread started");
    while (true)
    {
        std::deque<EventBatch> local;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            queue_not_empty_.wait(lk, [this] {
                return !queue_.empty() || !running_.load();
            });

            if (!running_.load() && queue_.empty())
                break;

            local.swap(queue_);
            if (metrics_)
                metrics_->setControllerQueueDepth(static_cast<double>(queue_.size()));
            debugf(*logger_, "consumerLoop() dequeued {} batch(es), queue now empty", local.size());
        }
        queue_not_full_.notify_all();

        for (auto& batch : local)
        {
            if (force_quit_.load())
            {
                debugf(*logger_, "consumerLoop() force_quit set — aborting drain, {} batch(es) discarded",
                       local.size());
                break;
            }
            dispatchToWriters(std::move(batch));
        }

        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(queue_log_mutex_);
            if (now - last_queue_log_time_ >= std::chrono::seconds(10))
            {
                last_queue_log_time_ = now;
                std::size_t depth;
                {
                    std::lock_guard<std::mutex> qlk(queue_mutex_);
                    depth = queue_.size();
                }
                infof(*logger_, "Controller main queue depth: {}", depth);
            }
        }
    }
    infof(*logger_, "Consumer thread exiting");
}

void MLDPPVXSController::dispatchToWriters(EventBatch batch_values)
{
    const std::string rootSource = getRootSourceName(batch_values);
    const std::size_t n = writers_.size();

    std::vector<std::future<bool>> futures;
    std::vector<std::size_t>       writer_indices;
    futures.reserve(n);
    writer_indices.reserve(n);

    for (std::size_t i = 0; i < n; ++i)
    {
        if (!route_table_.accepts(writers_[i]->name(), batch_values.reader_name))
            continue;

        if (!route_table_.acceptsSource(writers_[i]->name(), rootSource))
            continue;

        if (!writers_[i]->acceptsPayload(batch_values.payload))
            continue;

        auto*      writerPtr = writers_[i].get();
        EventBatch batchCopy = batch_values;
        futures.push_back(
            thread_pool_->submit_task([writerPtr, b = std::move(batchCopy)]() mutable -> bool
                                      {
                                          return writerPtr->push(std::move(b));
                                      }));
        writer_indices.push_back(i);
    }

    for (std::size_t fi = 0; fi < futures.size(); ++fi)
    {
        const bool ok = futures[fi].get();
        if (!ok)
        {
            warnf(*logger_, "Writer '{}' rejected batch for source {}",
                  writers_[writer_indices[fi]]->name(), rootSource);
        }
    }
}
