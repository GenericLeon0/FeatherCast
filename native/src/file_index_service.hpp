#pragma once

#include "file_content.hpp"
#include "storage.hpp"

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace feathercast::files {

struct IndexRequest {
  std::uint64_t generation = 0;
  std::vector<std::wstring> roots;
  std::size_t limit = 5000;
  bool contentEnabled = false;
};

struct IndexStatus {
  std::uint64_t generation = 0;
  std::vector<storage::FileIndexEntry> entries;
  std::size_t indexedContentFiles = 0;
  long long indexedContentBytes = 0;
  std::size_t unavailableRoots = 0;
  bool live = false;
  std::wstring message;
};

class FileIndexService {
 public:
  using ResultSink = std::function<void(IndexStatus)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  explicit FileIndexService(ResultSink sink = {}, ErrorSink errors = {});
  ~FileIndexService();
  FileIndexService(const FileIndexService&) = delete;
  FileIndexService& operator=(const FileIndexService&) = delete;

  void Start();
  void Stop();
  bool Reconfigure(IndexRequest request);
  bool Rebuild();
  bool IsCurrent(std::uint64_t generation) const;

 private:
  struct Watcher;
  void WorkerLoop(std::stop_token token);
  IndexStatus Scan(const IndexRequest& request, std::stop_token token) const;
  void RestartWatchers(const std::vector<std::wstring>& roots);
  void StopWatchers();
  void ScheduleWatchRefresh(bool restartWatchers = false);

  ResultSink sink_;
  ErrorSink errors_;
  std::jthread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<IndexRequest> request_;
  std::vector<std::unique_ptr<Watcher>> watchers_;
  std::mutex watchersMutex_;
  std::atomic<std::uint64_t> currentGeneration_ = 0;
  bool stopping_ = false;
  bool rebuildPending_ = false;
  bool restartWatchersPending_ = false;
  std::chrono::steady_clock::time_point rebuildAfter_{};
  std::chrono::seconds retryDelay_{2};
};

}  // namespace feathercast::files
