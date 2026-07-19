#pragma once

#include "background_executor.hpp"

#include <condition_variable>
#include <atomic>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <deque>
#include <cstdint>

struct IWICImagingFactory;

namespace feathercast::runtime {

struct DecodedIcon {
  std::wstring key;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::vector<std::uint8_t> pixels;
};

std::optional<DecodedIcon> DecodePngIcon(
    ::IWICImagingFactory* factory, const std::filesystem::path& path,
    const std::wstring& key);

template <typename Result>
class SingleOperationService {
 public:
  using Work = std::function<std::optional<Result>(std::stop_token)>;
  using Completed = std::function<void(Result)>;
  using Failed = std::function<void(std::exception_ptr)>;

  explicit SingleOperationService(Completed completed = {}, Failed failed = {})
      : completed_(std::move(completed)), failed_(std::move(failed)) {}

  ~SingleOperationService() { Stop(); }

  SingleOperationService(const SingleOperationService&) = delete;
  SingleOperationService& operator=(const SingleOperationService&) = delete;

  bool Run(Work work) {
    std::jthread finished;
    {
      std::lock_guard lock(mutex_);
      if (running_ || stopping_) return false;
      finished = std::move(worker_);
    }
    if (finished.joinable()) finished.join();
    {
      std::lock_guard lock(mutex_);
      if (running_ || stopping_) return false;
      running_ = true;
      worker_ = std::jthread(
          [this, work = std::move(work)](std::stop_token token) mutable {
            try {
              auto result = work(token);
              running_ = false;
              if (result && !token.stop_requested() && completed_) {
                completed_(std::move(*result));
              }
            } catch (...) {
              running_ = false;
              if (!token.stop_requested() && failed_) {
                failed_(std::current_exception());
              }
            }
          });
    }
    return true;
  }

  bool RunTask(std::function<void(std::stop_token)> task) {
    return Run([task = std::move(task)](std::stop_token token)
                   -> std::optional<Result> {
      task(token);
      return std::nullopt;
    });
  }

  void Stop() {
    std::jthread worker;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      worker = std::move(worker_);
    }
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    running_ = false;
    std::lock_guard lock(mutex_);
    stopping_ = false;
  }

  [[nodiscard]] bool Running() const noexcept { return running_; }

 private:
  Completed completed_;
  Failed failed_;
  mutable std::mutex mutex_;
  std::jthread worker_;
  std::atomic<bool> running_ = false;
  bool stopping_ = false;
};

template <typename Result>
using UpdateService = SingleOperationService<Result>;

template <typename Result>
using CurrencyService = SingleOperationService<Result>;

class LaunchService {
 public:
  using Task = background::Executor::Task;
  using ErrorHandler = background::Executor::ErrorHandler;

  void Start(std::size_t workers = 2, ErrorHandler errorHandler = {});
  void Stop();
  bool Submit(Task task);

 private:
  background::Executor executor_;
};

class IconResolver {
 public:
  using Resolve = std::function<std::optional<DecodedIcon>(
      const std::wstring&, std::stop_token)>;
  using Completed = std::function<void(DecodedIcon)>;
  using Failed = std::function<void(std::exception_ptr)>;

  explicit IconResolver(Completed completed = {}, Failed failed = {});
  ~IconResolver();

  IconResolver(const IconResolver&) = delete;
  IconResolver& operator=(const IconResolver&) = delete;

  void Start(std::size_t workers, Resolve resolve);
  void Stop();
  bool Queue(std::wstring key);
  void ClearPending();

 private:
  void WorkerLoop(std::stop_token stopToken);

  Completed completed_;
  Failed failed_;
  Resolve resolve_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::set<std::wstring> pending_;
  std::deque<std::wstring> jobs_;
  std::vector<std::jthread> workers_;
  bool stopping_ = false;
};

}  // namespace feathercast::runtime
