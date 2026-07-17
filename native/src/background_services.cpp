#include "background_services.hpp"

#include <objbase.h>

#include <algorithm>
#include <utility>

namespace feathercast::runtime {

void LaunchService::Start(std::size_t workers, ErrorHandler errorHandler) {
  executor_.Start(workers, std::move(errorHandler));
}

void LaunchService::Stop() {
  executor_.Shutdown();
}

bool LaunchService::Submit(Task task) {
  return executor_.Submit(std::move(task));
}

IconResolver::IconResolver(Completed completed)
    : completed_(std::move(completed)) {}

IconResolver::~IconResolver() {
  Stop();
}

void IconResolver::Start(std::size_t workers, Resolve resolve) {
  std::lock_guard lock(mutex_);
  if (!workers_.empty()) return;
  stopping_ = false;
  resolve_ = std::move(resolve);
  workers = std::max<std::size_t>(1, workers);
  workers_.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    workers_.emplace_back(
        [this](std::stop_token token) { WorkerLoop(token); });
  }
}

void IconResolver::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (workers_.empty()) return;
    stopping_ = true;
    jobs_.clear();
    pending_.clear();
    for (auto& worker : workers_) worker.request_stop();
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  std::lock_guard lock(mutex_);
  workers_.clear();
  resolve_ = {};
  stopping_ = false;
}

bool IconResolver::Queue(std::wstring key) {
  if (key.empty()) return false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || workers_.empty() || !resolve_) return false;
    if (!pending_.insert(key).second) return true;
    jobs_.push_back(std::move(key));
  }
  cv_.notify_one();
  return true;
}

void IconResolver::ClearPending() {
  std::lock_guard lock(mutex_);
  jobs_.clear();
  pending_.clear();
}

void IconResolver::WorkerLoop(std::stop_token stopToken) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  for (;;) {
    std::wstring key;
    Resolve resolve;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return !jobs_.empty() || stopping_ || stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested()) break;
      key = std::move(jobs_.front());
      jobs_.pop_front();
      resolve = resolve_;
    }
    if (!stopToken.stop_requested()) resolve(key, stopToken);
    {
      std::lock_guard lock(mutex_);
      pending_.erase(key);
    }
    if (!stopToken.stop_requested() && completed_) completed_(std::move(key));
  }
  CoUninitialize();
}

}  // namespace feathercast::runtime
