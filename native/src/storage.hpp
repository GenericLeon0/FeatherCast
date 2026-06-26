#pragma once

#include "sqlite3.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::storage {

struct FileIndexEntry {
  std::wstring path;
  std::wstring name;
  bool isDirectory = false;
  std::wstring iconKey;
  long long lastWriteTime = 0;
  long long size = 0;
  long long indexedAt = 0;
};

struct ClipboardEntry {
  long long id = 0;
  std::wstring text;
  std::wstring preview;
  long long capturedAt = 0;
};

class Statement {
 public:
  Statement() = default;
  ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  bool Prepare(sqlite3* db, const char* sql) {
    return sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
  }

  sqlite3_stmt* get() const { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

class Storage {
 public:
  Storage() = default;
  ~Storage() { Close(); }
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  bool Open(const std::filesystem::path& path) {
    Close();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
      Close();
      return false;
    }

    if (!Exec("PRAGMA journal_mode=WAL;") ||
        !Exec("PRAGMA synchronous=NORMAL;") ||
        !Exec("CREATE TABLE IF NOT EXISTS file_index ("
              "path TEXT PRIMARY KEY,"
              "name TEXT NOT NULL,"
              "is_directory INTEGER NOT NULL,"
              "icon_key TEXT NOT NULL,"
              "last_write_time INTEGER NOT NULL,"
              "size INTEGER NOT NULL,"
              "indexed_at INTEGER NOT NULL"
              ");") ||
        !Exec("CREATE TABLE IF NOT EXISTS clipboard_history ("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "text TEXT NOT NULL,"
              "preview TEXT NOT NULL,"
              "captured_at INTEGER NOT NULL"
              ");") ||
        !Exec("CREATE INDEX IF NOT EXISTS idx_clipboard_history_recent "
              "ON clipboard_history(captured_at DESC, id DESC);")) {
      Close();
      return false;
    }
    return true;
  }

  void Close() {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  bool IsOpen() const { return db_ != nullptr; }

  std::vector<FileIndexEntry> LoadFileIndex(size_t limit = 20000) const {
    std::vector<FileIndexEntry> out;
    if (!db_) return out;

    Statement stmt;
    if (!stmt.Prepare(db_,
                      "SELECT path, name, is_directory, icon_key, last_write_time, size, indexed_at "
                      "FROM file_index ORDER BY name COLLATE NOCASE LIMIT ?;")) {
      return out;
    }
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(limit));

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      FileIndexEntry entry;
      entry.path = ColumnText(stmt.get(), 0);
      entry.name = ColumnText(stmt.get(), 1);
      entry.isDirectory = sqlite3_column_int(stmt.get(), 2) != 0;
      entry.iconKey = ColumnText(stmt.get(), 3);
      entry.lastWriteTime = sqlite3_column_int64(stmt.get(), 4);
      entry.size = sqlite3_column_int64(stmt.get(), 5);
      entry.indexedAt = sqlite3_column_int64(stmt.get(), 6);
      out.push_back(std::move(entry));
    }
    return out;
  }

  bool ReplaceFileIndex(const std::vector<FileIndexEntry>& entries) {
    if (!db_) return false;
    if (!Exec("BEGIN IMMEDIATE;")) return false;
    if (!Exec("DELETE FROM file_index;")) {
      Exec("ROLLBACK;");
      return false;
    }

    Statement stmt;
    if (!stmt.Prepare(db_,
                      "INSERT INTO file_index "
                      "(path, name, is_directory, icon_key, last_write_time, size, indexed_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);")) {
      Exec("ROLLBACK;");
      return false;
    }

    for (const auto& entry : entries) {
      sqlite3_reset(stmt.get());
      sqlite3_clear_bindings(stmt.get());
      BindText(stmt.get(), 1, entry.path);
      BindText(stmt.get(), 2, entry.name);
      sqlite3_bind_int(stmt.get(), 3, entry.isDirectory ? 1 : 0);
      BindText(stmt.get(), 4, entry.iconKey);
      sqlite3_bind_int64(stmt.get(), 5, entry.lastWriteTime);
      sqlite3_bind_int64(stmt.get(), 6, entry.size);
      sqlite3_bind_int64(stmt.get(), 7, entry.indexedAt);
      if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return false;
      }
    }

    if (!Exec("COMMIT;")) {
      Exec("ROLLBACK;");
      return false;
    }
    return true;
  }

  std::vector<ClipboardEntry> LoadClipboardHistory(size_t limit = 50) const {
    std::vector<ClipboardEntry> out;
    if (!db_) return out;

    Statement stmt;
    if (!stmt.Prepare(db_,
                      "SELECT id, text, preview, captured_at FROM clipboard_history "
                      "ORDER BY captured_at DESC, id DESC LIMIT ?;")) {
      return out;
    }
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(limit));

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      ClipboardEntry entry;
      entry.id = sqlite3_column_int64(stmt.get(), 0);
      entry.text = ColumnText(stmt.get(), 1);
      entry.preview = ColumnText(stmt.get(), 2);
      entry.capturedAt = sqlite3_column_int64(stmt.get(), 3);
      out.push_back(std::move(entry));
    }
    return out;
  }

  std::optional<ClipboardEntry> AddClipboardEntry(const std::wstring& text,
                                                  const std::wstring& preview,
                                                  long long capturedAt,
                                                  size_t limit = 50) {
    if (!db_ || text.empty()) return std::nullopt;
    if (!Exec("BEGIN IMMEDIATE;")) return std::nullopt;

    {
      Statement remove;
      if (!remove.Prepare(db_, "DELETE FROM clipboard_history WHERE text = ?;")) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
      BindText(remove.get(), 1, text);
      if (sqlite3_step(remove.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
    }

    {
      Statement insert;
      if (!insert.Prepare(db_,
                          "INSERT INTO clipboard_history (text, preview, captured_at) "
                          "VALUES (?, ?, ?);")) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
      BindText(insert.get(), 1, text);
      BindText(insert.get(), 2, preview);
      sqlite3_bind_int64(insert.get(), 3, capturedAt);
      if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
    }

    const long long id = sqlite3_last_insert_rowid(db_);

    {
      Statement prune;
      if (!prune.Prepare(db_,
                         "DELETE FROM clipboard_history WHERE id NOT IN ("
                         "SELECT id FROM clipboard_history ORDER BY captured_at DESC, id DESC LIMIT ?"
                         ");")) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
      sqlite3_bind_int64(prune.get(), 1, static_cast<sqlite3_int64>(limit));
      if (sqlite3_step(prune.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
    }

    if (!Exec("COMMIT;")) {
      Exec("ROLLBACK;");
      return std::nullopt;
    }

    return ClipboardEntry{id, text, preview, capturedAt};
  }

  bool ClearClipboardHistory() {
    return Exec("DELETE FROM clipboard_history;");
  }

 private:
  bool Exec(const char* sql) const {
    if (!db_) return false;
    return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  }

  static void BindText(sqlite3_stmt* stmt, int index, const std::wstring& value) {
    sqlite3_bind_text16(stmt, index, value.c_str(), static_cast<int>(value.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
  }

  static std::wstring ColumnText(sqlite3_stmt* stmt, int index) {
    const auto* raw = static_cast<const wchar_t*>(sqlite3_column_text16(stmt, index));
    const int bytes = sqlite3_column_bytes16(stmt, index);
    if (!raw || bytes <= 0) return L"";
    return std::wstring(raw, raw + bytes / static_cast<int>(sizeof(wchar_t)));
  }

  sqlite3* db_ = nullptr;
};

}  // namespace feathercast::storage
