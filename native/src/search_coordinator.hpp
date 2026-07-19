#pragma once

#include "app_types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <variant>

namespace feathercast::search {

using CoordinatorEvent =
    std::variant<app::ResultsCollection, app::SnapshotBuildResult>;

class SearchCoordinator {
 public:
  using Processor =
      std::function<app::ResultsCollection(const app::QueryRequest&)>;
  using ResultSink = std::function<void(app::ResultsCollection)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  explicit SearchCoordinator(ResultSink resultSink = {}, ErrorSink errorSink = {});
  ~SearchCoordinator();

  SearchCoordinator(const SearchCoordinator&) = delete;
  SearchCoordinator& operator=(const SearchCoordinator&) = delete;

  void Start(Processor processor);
  void Stop();
  bool Query(app::QueryRequest request);
  void Invalidate(std::uint64_t generation);

 private:
  void WorkerLoop(std::stop_token stopToken);

  ResultSink resultSink_;
  ErrorSink errorSink_;
  Processor processor_;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<app::QueryRequest> pending_;
  std::atomic<unsigned long long> latestGeneration_ = 0;
  bool stopping_ = false;
};

class SnapshotCoordinator {
 public:
  using Builder = std::function<std::shared_ptr<const app::SearchSnapshot>(
      const app::Settings&)>;
  using ResultSink = std::function<void(app::SnapshotBuildResult)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  explicit SnapshotCoordinator(ResultSink resultSink = {}, ErrorSink errorSink = {});
  ~SnapshotCoordinator();

  SnapshotCoordinator(const SnapshotCoordinator&) = delete;
  SnapshotCoordinator& operator=(const SnapshotCoordinator&) = delete;

  void Start(Builder builder);
  void Stop();
  bool UpdateCorpus(app::SnapshotBuildRequest request);

 private:
  void WorkerLoop(std::stop_token stopToken);

  ResultSink resultSink_;
  ErrorSink errorSink_;
  Builder builder_;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<app::SnapshotBuildRequest> pending_;
  std::atomic<std::uint64_t> latestRevision_ = 0;
  bool stopping_ = false;
};

}  // namespace feathercast::search
