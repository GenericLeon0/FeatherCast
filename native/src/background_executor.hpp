#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace feathercast::background {

class Executor {
 public:
  using Task = std::function<void(std::stop_token)>;
  using Completion = std::function<void()>;

  Executor() = default;
  ~Executor() { Shutdown(); }

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  void Start(size_t workerCount = 2) {
    std::lock_guard lock(mutex_);
    if (!workers_.empty()) return;
    stopping_ = false;
    drainOnShutdown_ = false;
    workerCount = std::max<size_t>(1, workerCount);
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
      workers_.emplace_back([this](std::stop_token stopToken) { WorkerLoop(stopToken); });
    }
  }

  bool Submit(Task task) {
    if (!task) return false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || workers_.empty()) return false;
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  bool Submit(Task task, Completion completion) {
    return Submit([task = std::move(task), completion = std::move(completion)](std::stop_token stopToken) {
      task(stopToken);
      if (!stopToken.stop_requested() && completion) completion();
    });
  }

  void Shutdown(bool drainPending = false) {
    {
      std::lock_guard lock(mutex_);
      if (workers_.empty()) return;
      stopping_ = true;
      drainOnShutdown_ = drainPending;
      if (!drainPending) {
        tasks_.clear();
        for (auto& worker : workers_) worker.request_stop();
      }
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
  }

 private:
  void WorkerLoop(std::stop_token stopToken) {
    for (;;) {
      Task task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
          return stopping_ || stopToken.stop_requested() || !tasks_.empty();
        });
        if (stopToken.stop_requested()) return;
        if (stopping_ && (!drainOnShutdown_ || tasks_.empty())) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task(stopToken);
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> tasks_;
  std::vector<std::jthread> workers_;
  bool stopping_ = false;
  bool drainOnShutdown_ = false;
};

using BackgroundExecutor = Executor;

}  // namespace feathercast::background
