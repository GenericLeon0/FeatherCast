#include "search_coordinator.hpp"

#include <utility>

namespace feathercast::search {

SearchCoordinator::SearchCoordinator(ResultSink resultSink)
    : resultSink_(std::move(resultSink)) {}

SearchCoordinator::~SearchCoordinator() {
  Stop();
}

void SearchCoordinator::Start(Processor processor) {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  processor_ = std::move(processor);
  stopping_ = false;
  worker_ = std::jthread(
      [this](std::stop_token token) { WorkerLoop(token); });
}

void SearchCoordinator::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    pending_.reset();
    worker_.request_stop();
  }
  cv_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  processor_ = {};
  stopping_ = false;
}

bool SearchCoordinator::Query(app::QueryRequest request) {
  latestGeneration_.store(request.generation, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable() || !processor_) return false;
    pending_ = std::move(request);
  }
  cv_.notify_one();
  return true;
}

void SearchCoordinator::Invalidate(std::uint64_t generation) {
  latestGeneration_.store(generation, std::memory_order_release);
  std::lock_guard lock(mutex_);
  if (pending_ && pending_->generation != generation) pending_.reset();
}

void SearchCoordinator::WorkerLoop(std::stop_token stopToken) {
  for (;;) {
    app::QueryRequest request;
    Processor processor;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested()) return;
      request = std::move(*pending_);
      pending_.reset();
      processor = processor_;
    }
    request.latestGeneration = &latestGeneration_;
    auto result = processor(request);
    if (request.generation !=
        latestGeneration_.load(std::memory_order_acquire)) {
      continue;
    }
    if (resultSink_) resultSink_(std::move(result));
  }
}

SnapshotCoordinator::SnapshotCoordinator(ResultSink resultSink)
    : resultSink_(std::move(resultSink)) {}

SnapshotCoordinator::~SnapshotCoordinator() {
  Stop();
}

void SnapshotCoordinator::Start(Builder builder) {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  builder_ = std::move(builder);
  stopping_ = false;
  worker_ = std::jthread(
      [this](std::stop_token token) { WorkerLoop(token); });
}

void SnapshotCoordinator::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    pending_.reset();
    worker_.request_stop();
  }
  cv_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  builder_ = {};
  stopping_ = false;
}

bool SnapshotCoordinator::UpdateCorpus(app::SnapshotBuildRequest request) {
  latestRevision_.store(request.revision, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable() || !builder_) return false;
    pending_ = std::move(request);
  }
  cv_.notify_one();
  return true;
}

void SnapshotCoordinator::WorkerLoop(std::stop_token stopToken) {
  for (;;) {
    app::SnapshotBuildRequest request;
    Builder builder;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested()) return;
      request = std::move(*pending_);
      pending_.reset();
      builder = builder_;
    }
    auto snapshot = builder(request.settings);
    if (request.revision !=
        latestRevision_.load(std::memory_order_acquire)) {
      continue;
    }
    if (resultSink_) {
      resultSink_({request.revision, std::move(snapshot)});
    }
  }
}

}  // namespace feathercast::search
