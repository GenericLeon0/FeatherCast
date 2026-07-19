#pragma once

#include "app_types.hpp"
#include "core.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct sqlite3;

namespace feathercast::files {

struct FileQuery {
  unsigned long long generation = 0;
  std::wstring terms;
  int limit = 200;
  bool contentEnabled = false;
};

class FileSearchService {
 public:
  using ResultSink = std::function<void(app::ResultsCollection)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  FileSearchService(std::filesystem::path databasePath, ResultSink sink = {},
                    ErrorSink errors = {});
  ~FileSearchService();
  FileSearchService(const FileSearchService&) = delete;
  FileSearchService& operator=(const FileSearchService&) = delete;

  void Start();
  void Stop();
  void UpdateFiles(std::vector<app::AppEntry> files);
  bool Query(FileQuery query);
  void Invalidate(unsigned long long generation);

 private:
  struct Corpus;
  void WorkerLoop(std::stop_token token);
  app::ResultsCollection Compute(const FileQuery& query);
  bool EnsureDatabase();
  std::vector<std::wstring> QueryContent(const std::wstring& terms,
                                         std::size_t limit);

  std::filesystem::path databasePath_;
  ResultSink sink_;
  ErrorSink errors_;
  sqlite3* database_ = nullptr;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<FileQuery> pending_;
  std::shared_ptr<const Corpus> corpus_;
  std::atomic<unsigned long long> generation_ = 0;
  bool stopping_ = false;
};

}  // namespace feathercast::files
