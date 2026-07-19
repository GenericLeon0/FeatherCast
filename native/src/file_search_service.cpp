#include "file_search_service.hpp"

#include "extension_protocol.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <utility>

namespace feathercast::files {
namespace {

app::DisplayItem Display(app::AppEntry entry, bool contentMatch = false) {
  entry.fileContentMatch = contentMatch;
  app::DisplayItem item;
  item.app = std::move(entry);
  item.commandDetail = contentMatch ? L"Content match" : L"File or folder";
  return item;
}

core::SearchItem Searchable(const app::AppEntry& entry) {
  core::SearchItem item;
  item.id = entry.id;
  item.path = entry.path;
  item.name = entry.name;
  item.source = L"file";
  item.kind = L"file";
  item.targetPath = entry.path;
  return item;
}

std::wstring NormalizePath(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

std::string FtsExpression(const std::wstring& terms) {
  std::string out;
  std::wstring token;
  auto flush = [&] {
    if (token.empty()) return;
    if (!out.empty()) out += " AND ";
    out += '"';
    for (const char ch : extensions::WideToUtf8(token)) {
      if (ch == '"') out += "\"\"";
      else out += ch;
    }
    out += "\"*";
    token.clear();
  };
  for (const wchar_t ch : terms) {
    if (std::iswalnum(ch) || ch == L'_') token.push_back(ch);
    else flush();
  }
  flush();
  return out;
}

}  // namespace

struct FileSearchService::Corpus {
  std::vector<app::AppEntry> files;
  std::vector<core::PreparedSearchItem> prepared;
  std::map<std::wstring, std::size_t> byPath;
};

FileSearchService::FileSearchService(std::filesystem::path databasePath,
                                     ResultSink sink, ErrorSink errors)
    : databasePath_(std::move(databasePath)),
      sink_(std::move(sink)),
      errors_(std::move(errors)),
      corpus_(std::make_shared<Corpus>()) {}

FileSearchService::~FileSearchService() { Stop(); }

void FileSearchService::Start() {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  stopping_ = false;
  worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
}

void FileSearchService::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    pending_.reset();
    worker_.request_stop();
  }
  cv_.notify_all();
  worker_.join();
  if (database_) sqlite3_close(database_);
  database_ = nullptr;
  std::lock_guard lock(mutex_);
  stopping_ = false;
}

void FileSearchService::UpdateFiles(std::vector<app::AppEntry> files) {
  auto corpus = std::make_shared<Corpus>();
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
    if (left.fileLastWriteTime != right.fileLastWriteTime) {
      return left.fileLastWriteTime > right.fileLastWriteTime;
    }
    return left.path < right.path;
  });
  corpus->files = std::move(files);
  corpus->prepared.reserve(corpus->files.size());
  for (std::size_t index = 0; index < corpus->files.size(); ++index) {
    corpus->prepared.push_back(core::PrepareSearchItem(Searchable(corpus->files[index])));
    corpus->byPath[NormalizePath(corpus->files[index].path)] = index;
  }
  std::lock_guard lock(mutex_);
  corpus_ = std::move(corpus);
}

bool FileSearchService::Query(FileQuery query) {
  generation_.store(query.generation, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable()) return false;
    pending_ = std::move(query);
  }
  cv_.notify_one();
  return true;
}

void FileSearchService::Invalidate(unsigned long long generation) {
  generation_.store(generation, std::memory_order_release);
  std::lock_guard lock(mutex_);
  pending_.reset();
}

void FileSearchService::WorkerLoop(std::stop_token token) {
  for (;;) {
    FileQuery query;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return stopping_ || token.stop_requested() || pending_.has_value();
      });
      if (stopping_ || token.stop_requested()) return;
      query = std::move(*pending_);
      pending_.reset();
    }
    try {
      auto result = Compute(query);
      if (!token.stop_requested() &&
          generation_.load(std::memory_order_acquire) == query.generation &&
          sink_) {
        sink_(std::move(result));
      }
    } catch (...) {
      if (errors_) errors_(std::current_exception());
    }
  }
}

app::ResultsCollection FileSearchService::Compute(const FileQuery& query) {
  app::ResultsCollection result;
  result.generation = query.generation;
  std::shared_ptr<const Corpus> corpus;
  {
    std::lock_guard lock(mutex_);
    corpus = corpus_;
  }
  if (!corpus) return result;

  std::set<std::wstring> used;
  std::vector<app::DisplayItem> metadata;
  if (core::Trim(query.terms).empty()) {
    for (std::size_t i = 0; i < corpus->files.size() &&
                            metadata.size() < static_cast<std::size_t>(query.limit);
         ++i) {
      metadata.push_back(Display(corpus->files[i]));
      used.insert(NormalizePath(corpus->files[i].path));
    }
  } else {
    core::SearchOptions options;
    options.limit = static_cast<std::size_t>(query.limit);
    options.generation = query.generation;
    options.latestGeneration = &generation_;
    const auto order = core::SearchPrepared(query.terms, corpus->prepared, {}, options);
    for (const auto index : order) {
      metadata.push_back(Display(corpus->files[index]));
      used.insert(NormalizePath(corpus->files[index].path));
    }
  }
  if (!metadata.empty()) {
    result.sections.push_back(
        {core::Trim(query.terms).empty() ? L"Recently modified" : L"Names & paths",
         metadata});
    result.flatItems.insert(result.flatItems.end(), metadata.begin(), metadata.end());
  }

  if (query.contentEnabled && !core::Trim(query.terms).empty()) {
    std::vector<app::DisplayItem> content;
    const auto totalLimit = static_cast<std::size_t>(std::max(0, query.limit));
    const auto ftsLimit = std::max<std::size_t>(40, totalLimit * 2);
    for (const auto& path : QueryContent(query.terms, ftsLimit)) {
      if (result.flatItems.size() + content.size() >= totalLimit) break;
      const auto normalized = NormalizePath(path);
      if (used.contains(normalized)) continue;
      const auto found = corpus->byPath.find(normalized);
      if (found == corpus->byPath.end()) continue;
      used.insert(normalized);
      content.push_back(Display(corpus->files[found->second], true));
    }
    if (!content.empty()) {
      result.sections.push_back({L"Content matches", content});
      result.flatItems.insert(result.flatItems.end(), content.begin(), content.end());
    }
  }
  return result;
}

bool FileSearchService::EnsureDatabase() {
  if (database_) return true;
  const auto utf8 = extensions::WideToUtf8(databasePath_.wstring());
  if (sqlite3_open_v2(utf8.c_str(), &database_,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) !=
      SQLITE_OK) {
    if (database_) sqlite3_close(database_);
    database_ = nullptr;
    return false;
  }
  sqlite3_busy_timeout(database_, 3000);
  return true;
}

std::vector<std::wstring> FileSearchService::QueryContent(
    const std::wstring& terms, std::size_t limit) {
  std::vector<std::wstring> paths;
  const std::string expression = FtsExpression(terms);
  if (expression.empty() || !EnsureDatabase()) return paths;
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(
          database_,
          "SELECT f.path FROM file_content_fts c JOIN file_index f ON f.id=c.rowid "
          "WHERE file_content_fts MATCH ? ORDER BY bm25(file_content_fts) LIMIT ?;",
          -1, &statement, nullptr) != SQLITE_OK) {
    return paths;
  }
  sqlite3_bind_text(statement, 1, expression.c_str(),
                    static_cast<int>(expression.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(limit));
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto* raw = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
    const int bytes = sqlite3_column_bytes16(statement, 0);
    if (raw && bytes > 0) {
      paths.emplace_back(raw, raw + bytes / static_cast<int>(sizeof(wchar_t)));
    }
  }
  sqlite3_finalize(statement);
  return paths;
}

}  // namespace feathercast::files
