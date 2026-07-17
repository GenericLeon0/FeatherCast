#include "discovery_service.hpp"

#include <utility>

namespace feathercast::discovery_runtime {

DiscoveryService::DiscoveryService(ResultSink resultSink)
    : resultSink_(std::move(resultSink)) {}

DiscoveryService::~DiscoveryService() {
  Stop();
}

void DiscoveryService::Start(Worker worker) {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  workerFunction_ = std::move(worker);
  stopping_ = false;
  worker_ = std::jthread(
      [this](std::stop_token token) { WorkerLoop(token); });
}

void DiscoveryService::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    pending_.reset();
    latestGeneration_.fetch_add(1, std::memory_order_acq_rel);
    worker_.request_stop();
  }
  cv_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  workerFunction_ = {};
  stopping_ = false;
}

bool DiscoveryService::Refresh(app::DiscoveryRequest request) {
  latestGeneration_.store(request.generation, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable() || !workerFunction_) return false;
    pending_ = std::move(request);
  }
  cv_.notify_one();
  return true;
}

std::uint64_t DiscoveryService::Cancel() {
  const auto generation =
      latestGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
  {
    std::lock_guard lock(mutex_);
    pending_.reset();
  }
  cv_.notify_all();
  return generation;
}

bool DiscoveryService::IsCurrent(std::uint64_t generation) const {
  return latestGeneration_.load(std::memory_order_acquire) == generation;
}

void DiscoveryService::WorkerLoop(std::stop_token stopToken) {
  for (;;) {
    app::DiscoveryRequest request;
    Worker worker;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested()) return;
      request = std::move(*pending_);
      pending_.reset();
      worker = workerFunction_;
    }
    auto result = worker(request, stopToken);
    if (!result || stopToken.stop_requested() ||
        !IsCurrent(request.generation)) {
      continue;
    }
    if (resultSink_) resultSink_(std::move(*result));
  }
}

}  // namespace feathercast::discovery_runtime
