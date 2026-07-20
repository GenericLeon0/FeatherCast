#include "app_types.hpp"
#include "file_content.hpp"
#include "file_index_service.hpp"
#include "file_search_service.hpp"
#include "preview_service.hpp"
#include "search_scope.hpp"
#include "search_pipeline.hpp"
#include "storage.hpp"
#include "test_framework.hpp"

#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

void WriteBytes(const std::filesystem::path& path,
                const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary);
  assert(output.good());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  assert(output.good());
}

int Scalar(sqlite3* database, const char* sql) {
  sqlite3_stmt* statement = nullptr;
  assert(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const int value = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  return value;
}

std::string ScalarText(sqlite3* database, const char* sql) {
  sqlite3_stmt* statement = nullptr;
  assert(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const auto* value = sqlite3_column_text(statement, 0);
  const std::string text = value ? reinterpret_cast<const char*>(value) : "";
  sqlite3_finalize(statement);
  return text;
}

feathercast::storage::FileIndexEntry Entry(
    const std::filesystem::path& path, std::wstring content,
    long long indexedAt, long long writeTime) {
  feathercast::storage::FileIndexEntry entry;
  entry.path = path.wstring();
  entry.name = path.filename().wstring();
  entry.iconKey = L"file:" + entry.path;
  entry.lastWriteTime = writeTime;
  entry.size = 64;
  entry.indexedAt = indexedAt;
  entry.root = path.parent_path().wstring();
  entry.contentState = static_cast<int>(
      feathercast::file_content::State::Indexed);
  entry.contentBytes = static_cast<long long>(content.size());
  entry.scanGeneration = 9;
  entry.contentText = std::move(content);
  return entry;
}

feathercast::app::AppEntry App(const std::filesystem::path& path,
                               long long writeTime) {
  feathercast::app::AppEntry app;
  app.id = L"file:" + path.wstring();
  app.name = path.filename().wstring();
  app.path = path.wstring();
  app.source = L"file";
  app.fileLastWriteTime = writeTime;
  return app;
}

}  // namespace

int main() {
  namespace content = feathercast::file_content;
  namespace scope = feathercast::search_scope;

  {
    const auto plain = scope::Parse(L"  report  ");
    assert(plain.scope == scope::Scope::All);
    assert(!plain.recognized);
    assert(plain.terms == L"report");

    const auto files = scope::Parse(L" @FiLeS\t quarterly report ");
    assert(files.scope == scope::Scope::Files);
    assert(files.recognized);
    assert(files.token == L"@files");
    assert(files.terms == L"quarterly report");
    assert(scope::Parse(L"@files").terms.empty());
    assert(scope::Token(scope::Scope::Files) == L"@files");
    assert(scope::Token(scope::Scope::All).empty());

    const auto allSuggestions = scope::Suggestions(L"@");
    assert(allSuggestions.size() == 7);
    const auto fileSuggestions = scope::Suggestions(L"@fi");
    assert(fileSuggestions.size() == 1);
    assert(fileSuggestions.front()->scope == scope::Scope::Files);
    assert(scope::Parse(L"@fi").suggestionMode);
    assert(scope::Suggestions(L"@filesx").empty());
    const auto incompleteWithTerms = scope::Parse(L"@fi report");
    assert(!incompleteWithTerms.suggestionMode);
    assert(incompleteWithTerms.terms == L"@fi report");
  }

  {
    auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
    feathercast::app::DisplayItem app;
    app.app.id = L"app:common";
    app.app.name = L"Common App";
    app.app.source = L"shortcut";
    feathercast::core::SearchItem appSearch;
    appSearch.id = app.app.id;
    appSearch.name = app.app.name;
    appSearch.kind = L"app";
    appSearch.source = app.app.source;
    snapshot->pool.push_back(app);
    snapshot->searchItems.push_back(
        feathercast::core::PrepareSearchItem(appSearch));

    feathercast::app::DisplayItem command;
    command.isCommand = true;
    command.command = feathercast::app::CommandKind::Settings;
    command.commandName = L"Common Command";
    feathercast::core::SearchItem commandSearch;
    commandSearch.id = L"command:common";
    commandSearch.name = command.commandName;
    commandSearch.kind = L"command";
    commandSearch.source = L"command";
    snapshot->pool.push_back(command);
    snapshot->searchItems.push_back(
        feathercast::core::PrepareSearchItem(commandSearch));

    feathercast::app::QueryRequest request;
    request.generation = 3;
    request.query = L"common";
    request.limit = 20;
    request.snapshot = snapshot;
    request.scope = scope::Scope::Apps;
    auto scopedResults = feathercast::search_pipeline::ComputeResults(request);
    assert(scopedResults.flatItems.size() == 1);
    assert(scopedResults.flatItems.front().app.id == app.app.id);
    request.scope = scope::Scope::Commands;
    scopedResults = feathercast::search_pipeline::ComputeResults(request);
    assert(scopedResults.flatItems.size() == 1);
    assert(scopedResults.flatItems.front().isCommand);

    request.scope = scope::Scope::All;
    request.query = L"@";
    scopedResults = feathercast::search_pipeline::ComputeResults(request);
    assert(scopedResults.sections.size() == 1);
    assert(scopedResults.sections.front().title == L"Search scopes");
    assert(scopedResults.flatItems.size() == 7);
  }

  wchar_t temporary[MAX_PATH]{};
  assert(GetTempPathW(MAX_PATH, temporary) > 0);
  const auto root = std::filesystem::path(temporary) /
                    (L"FeatherCastSearchFilesTests-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
  std::error_code error;
  std::filesystem::create_directories(root, error);
  assert(!error);

  {
    assert(content::Supports(root / L"NOTE.TXT"));
    assert(!content::Supports(root / L"image.PNG"));
    assert(content::IsImage(root / L"image.PNG"));
    assert(!content::Supports(root / L"binary.exe"));

    const auto utf8 = root / L"utf8.txt";
    WriteBytes(utf8, {0xef, 0xbb, 0xbf, 'G', 'r', 0xc3, 0xbc, 0xc3, 0x9f,
                      'e'});
    const auto utf8Result = content::Extract(utf8);
    assert(utf8Result.state == content::State::Indexed);
    assert(utf8Result.text == L"Gr\u00fc\u00dfe");

    const auto utf16Le = root / L"utf16-le.txt";
    WriteBytes(utf16Le, {0xff, 0xfe, 'A', 0, 'B', 0});
    assert(content::Extract(utf16Le).text == L"AB");
    const auto utf16Be = root / L"utf16-be.txt";
    WriteBytes(utf16Be, {0xfe, 0xff, 0, 'A', 0, 'B'});
    assert(content::Extract(utf16Be).text == L"AB");

    const auto binary = root / L"binary.txt";
    WriteBytes(binary, {'A', 0, 'B'});
    assert(content::Extract(binary).state == content::State::Binary);
    assert(content::Extract(utf8, 3).state == content::State::TooLarge);
    assert(content::Extract(root / L"missing.txt").state ==
           content::State::Unavailable);
    assert(content::Extract(root / L"binary.exe").state ==
           content::State::Unsupported);
  }

  {
    const auto indexedRoot = root / L"indexed-root";
    auto deep = indexedRoot;
    for (int level = 0; level < 6; ++level) {
      deep /= L"level-" + std::to_wstring(level);
    }
    std::filesystem::create_directories(deep, error);
    assert(!error);
    const auto deepFile = deep / L"deep.txt";
    WriteBytes(deepFile, {'d', 'e', 'e', 'p'});

    std::mutex mutex;
    std::condition_variable ready;
    feathercast::files::IndexStatus latest;
    feathercast::files::FileIndexService index(
        [&](feathercast::files::IndexStatus status) {
          {
            std::lock_guard lock(mutex);
            latest = std::move(status);
          }
          ready.notify_all();
        });
    const auto contains = [&](const std::filesystem::path& path) {
      return std::any_of(latest.entries.begin(), latest.entries.end(),
                         [&](const auto& entry) {
                           return std::filesystem::path(entry.path) == path;
                         });
    };
    index.Start();
    assert(index.Reconfigure({11, {indexedRoot.wstring()}, 100, false}));
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(5),
                            [&] { return contains(deepFile); }));
    }

    const auto watched = deep / L"watched.txt";
    WriteBytes(watched, {'n', 'e', 'w'});
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(5),
                            [&] { return contains(watched); }));
    }
    const auto renamed = deep / L"renamed.txt";
    std::filesystem::rename(watched, renamed, error);
    assert(!error);
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(5), [&] {
        return contains(renamed) && !contains(watched);
      }));
    }
    std::filesystem::remove(renamed, error);
    assert(!error);
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(5),
                            [&] { return !contains(renamed); }));
    }
    index.Stop();
  }

  {
    const auto previewPath = root / L"preview.txt";
    std::vector<unsigned char> previewBytes(18000, 'x');
    constexpr std::string_view marker = "needle";
    std::copy(marker.begin(), marker.end(), previewBytes.begin() + 9000);
    WriteBytes(previewPath, previewBytes);

    std::mutex mutex;
    std::condition_variable ready;
    bool received = false;
    feathercast::preview::Result result;
    feathercast::preview::PreviewService preview(
        [&](feathercast::preview::Result value) {
          {
            std::lock_guard lock(mutex);
            result = std::move(value);
            received = true;
          }
          ready.notify_one();
        });
    preview.Start();
    assert(preview.Load({20, previewPath, L"needle"}));
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(3),
                            [&] { return received; }));
    }
    assert(result.generation == 20);
    assert(result.kind == feathercast::preview::Kind::Text);
    assert(result.text.size() <= 16 * 1024 / sizeof(wchar_t));
    assert(result.text.find(L"needle") != std::wstring::npos);

    std::filesystem::remove(previewPath, error);
    assert(!error);
    {
      std::lock_guard lock(mutex);
      received = false;
    }
    assert(preview.Load({21, previewPath, L""}));
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(3),
                            [&] { return received; }));
    }
    assert(result.generation == 21);
    assert(result.kind == feathercast::preview::Kind::Error);
    preview.Stop();
  }

  {
    const auto legacyPath = root / L"legacy-v2.db";
    sqlite3* database = nullptr;
    assert(sqlite3_open16(legacyPath.c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(
               database,
               "CREATE TABLE file_index (path TEXT PRIMARY KEY,name TEXT NOT NULL,"
               "is_directory INTEGER NOT NULL,icon_key TEXT NOT NULL,"
               "last_write_time INTEGER NOT NULL,size INTEGER NOT NULL,"
               "indexed_at INTEGER NOT NULL);"
               "INSERT INTO file_index VALUES"
               "('C:/legacy.txt','legacy.txt',0,'legacy',7,12,9);"
               "CREATE TABLE clipboard_history (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "text TEXT NOT NULL,preview TEXT NOT NULL,captured_at INTEGER NOT NULL,"
               "content_hash TEXT NOT NULL DEFAULT '',encrypted INTEGER NOT NULL DEFAULT 0);"
               "INSERT INTO clipboard_history(text,preview,captured_at,content_hash,encrypted) "
               "VALUES('preserved clipboard','preserved',42,'hash',0);"
               "PRAGMA user_version=2;",
               nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);

    feathercast::storage::Storage migrated;
    assert(migrated.Open(legacyPath));
    const auto migratedFiles = migrated.LoadFileIndex();
    assert(migratedFiles.size() == 1);
    assert(migratedFiles.front().id > 0);
    assert(migratedFiles.front().name == L"legacy.txt");
    const auto migratedClipboard = migrated.LoadClipboardHistory();
    assert(migratedClipboard.size() == 1);
    assert(migratedClipboard.front().text == L"preserved clipboard");
    migrated.Close();
    assert(std::filesystem::exists(legacyPath.wstring() + L".pre-v3.bak"));

    const auto futurePath = root / L"future.db";
    assert(sqlite3_open16(futurePath.c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database, "PRAGMA user_version=4;", nullptr, nullptr,
                        nullptr) == SQLITE_OK);
    sqlite3_close(database);
    feathercast::storage::Storage future;
    assert(!future.Open(futurePath));
    assert(sqlite3_open16(futurePath.c_str(), &database) == SQLITE_OK);
    assert(Scalar(database, "PRAGMA user_version;") == 4);
    sqlite3_close(database);
  }

  const auto databasePath = root / L"feathercast.db";
  const auto metadataMatch = root / L"sharedterm-name.txt";
  const auto contentOnly = root / L"content-only.txt";
  {
    feathercast::storage::Storage storage;
    assert(storage.Open(databasePath));
    auto first = Entry(metadataMatch, L"sharedterm alpha unique", 100, 20);
    auto second = Entry(contentOnly, L"sharedterm orphan content", 100, 10);
    assert(storage.UpdateFileIndex({first, second}));
    storage.Close();

    sqlite3* database = nullptr;
    assert(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK);
    assert(Scalar(database, "PRAGMA user_version;") == 3);
    const auto createSql = ScalarText(
        database,
        "SELECT sql FROM sqlite_master WHERE name='file_content_fts';");
    assert(createSql.find("contentless_delete=1") != std::string::npos);
    assert(Scalar(database,
                  "SELECT count(*) FROM file_content_fts WHERE "
                  "file_content_fts MATCH 'alpha';") == 1);
    assert(Scalar(database, "SELECT content IS NULL FROM file_content_fts LIMIT 1;") ==
           1);
    sqlite3_close(database);

    assert(storage.Open(databasePath));
    first.contentText = L"sharedterm beta replacement";
    first.contentBytes = static_cast<long long>(first.contentText.size());
    assert(storage.UpdateFileIndex({first, second}));
    storage.Close();

    assert(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK);
    assert(Scalar(database,
                  "SELECT count(*) FROM file_content_fts WHERE "
                  "file_content_fts MATCH 'alpha';") == 0);
    assert(Scalar(database,
                  "SELECT count(*) FROM file_content_fts WHERE "
                  "file_content_fts MATCH 'beta';") == 1);
    sqlite3_close(database);
  }

  {
    std::mutex mutex;
    std::condition_variable ready;
    bool received = false;
    feathercast::app::ResultsCollection results;
    feathercast::files::FileSearchService service(
        databasePath,
        [&](feathercast::app::ResultsCollection value) {
          {
            std::lock_guard lock(mutex);
            results = std::move(value);
            received = true;
          }
          ready.notify_one();
        });
    service.Start();
    service.UpdateFiles({App(metadataMatch, 20), App(contentOnly, 10)});
    assert(service.Query({7, L"sharedterm", 20, true}));
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, std::chrono::seconds(3),
                            [&] { return received; }));
    }
    service.Stop();
    assert(results.generation == 7);
    assert(results.sections.size() == 2);
    assert(results.sections[0].title == L"Names & paths");
    assert(results.sections[1].title == L"Content matches");
    assert(results.flatItems.size() == 2);
    assert(!results.flatItems[0].app.fileContentMatch);
    assert(results.flatItems[1].app.fileContentMatch);
    assert(results.flatItems[0].app.path != results.flatItems[1].app.path);
  }

  {
    feathercast::storage::Storage storage;
    assert(storage.Open(databasePath));
    assert(storage.ClearFileIndex());
    storage.Close();
    sqlite3* database = nullptr;
    assert(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK);
    assert(Scalar(database, "SELECT count(*) FROM file_index;") == 0);
    assert(Scalar(database, "SELECT count(*) FROM file_content_fts;") == 0);
    sqlite3_close(database);
  }

  std::filesystem::remove_all(root, error);
  assert(!error);
  return 0;
}
