#pragma once

#include "sqlite3.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>
#include <string>
#include <tuple>
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
  long long id = 0;
  std::wstring root;
  int contentState = 0;
  long long contentBytes = 0;
  long long scanGeneration = 0;
  std::wstring contentText;
};

struct ClipboardEntry {
  long long id = 0;
  std::wstring text;
  std::wstring preview;
  long long capturedAt = 0;
};

struct StorageError {
  int code = SQLITE_OK;
  std::string message;
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
    recoveredFromCorruption_ = false;
    quarantinedPath_.clear();
    lastError_ = {};
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    if (OpenOnce(path)) return true;
    const int failureCode = lastError_.code;
    Close();
    if (failureCode != SQLITE_CORRUPT && failureCode != SQLITE_NOTADB) return false;

    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    quarantinedPath_ = path;
    quarantinedPath_ += L".corrupt-" + std::to_wstring(timestamp);
    std::filesystem::rename(path, quarantinedPath_, ec);
    if (ec) {
      SetError(failureCode, "database integrity check failed and the file could not be quarantined");
      quarantinedPath_.clear();
      return false;
    }
    for (const wchar_t* suffix : {L"-wal", L"-shm"}) {
      std::filesystem::path sidecar = path;
      sidecar += suffix;
      std::filesystem::path quarantinedSidecar = quarantinedPath_;
      quarantinedSidecar += suffix;
      ec.clear();
      if (std::filesystem::exists(sidecar, ec)) {
        ec.clear();
        std::filesystem::rename(sidecar, quarantinedSidecar, ec);
      }
    }

    lastError_ = {};
    if (!OpenOnce(path)) return false;
    recoveredFromCorruption_ = true;
    return true;
  }

  bool RecoveredFromCorruption() const { return recoveredFromCorruption_; }
  const std::filesystem::path& QuarantinedPath() const { return quarantinedPath_; }
  const StorageError& LastError() const { return lastError_; }

 private:
  bool OpenOnce(const std::filesystem::path& path) {
    if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
      CaptureError();
      Close();
      return false;
    }

    sqlite3_busy_timeout(db_, 3000);
    if (!IntegrityCheck()) {
      Close();
      return false;
    }
    if (!Exec("PRAGMA journal_mode=WAL;") ||
        !Exec("PRAGMA synchronous=NORMAL;")) {
      Close();
      return false;
    }
    const int schemaVersion = ReadSchemaVersion();
    if (schemaVersion < 0 || schemaVersion > 3) {
      if (schemaVersion > 3) {
        SetError(SQLITE_ERROR, "database schema is newer than this FeatherCast build");
      }
      Close();
      return false;
    }
    if (schemaVersion < 3 &&
        (HasTable("clipboard_history") || HasTable("file_index")) &&
        !BackupBeforeMigration(path, schemaVersion < 2 ? 2 : 3)) {
      Close();
      return false;
    }
    if (!Exec("BEGIN IMMEDIATE;")) {
      Close();
      return false;
    }
    if (!Exec("CREATE TABLE IF NOT EXISTS file_index ("
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
              "ON clipboard_history(captured_at DESC, id DESC);") ||
        !MigrateSchema(schemaVersion) ||
        !Exec("CREATE VIRTUAL TABLE IF NOT EXISTS file_content_fts USING fts5("
              "content, content='', contentless_delete=1, "
              "tokenize='unicode61 remove_diacritics 2');") ||
        !Exec("PRAGMA user_version=3;") ||
        !Exec("COMMIT;")) {
      Exec("ROLLBACK;");
      Close();
      return false;
    }
    return true;
  }

 public:
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
                      "SELECT id, path, name, is_directory, icon_key, last_write_time, size, "
                      "indexed_at, root, content_state, content_bytes, scan_generation "
                      "FROM file_index ORDER BY name COLLATE NOCASE LIMIT ?;")) {
      return out;
    }
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(limit));

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      FileIndexEntry entry;
      entry.id = sqlite3_column_int64(stmt.get(), 0);
      entry.path = ColumnText(stmt.get(), 1);
      entry.name = ColumnText(stmt.get(), 2);
      entry.isDirectory = sqlite3_column_int(stmt.get(), 3) != 0;
      entry.iconKey = ColumnText(stmt.get(), 4);
      entry.lastWriteTime = sqlite3_column_int64(stmt.get(), 5);
      entry.size = sqlite3_column_int64(stmt.get(), 6);
      entry.indexedAt = sqlite3_column_int64(stmt.get(), 7);
      entry.root = ColumnText(stmt.get(), 8);
      entry.contentState = sqlite3_column_int(stmt.get(), 9);
      entry.contentBytes = sqlite3_column_int64(stmt.get(), 10);
      entry.scanGeneration = sqlite3_column_int64(stmt.get(), 11);
      out.push_back(std::move(entry));
    }
    return out;
  }

  bool UpdateFileIndex(const std::vector<FileIndexEntry>& entries) {
    if (!db_) return false;
    if (!Exec("BEGIN IMMEDIATE;")) return false;

    Statement stmt;
    if (!stmt.Prepare(db_,
                      "INSERT INTO file_index "
                      "(path, name, is_directory, icon_key, last_write_time, size, indexed_at, "
                      "root, content_state, content_bytes, scan_generation) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(path) DO UPDATE SET "
                      "name=excluded.name, is_directory=excluded.is_directory, "
                      "icon_key=excluded.icon_key, last_write_time=excluded.last_write_time, "
                      "size=excluded.size, indexed_at=excluded.indexed_at, root=excluded.root, "
                      "content_state=excluded.content_state, content_bytes=excluded.content_bytes, "
                      "scan_generation=excluded.scan_generation;")) {
      Exec("ROLLBACK;");
      return false;
    }
    Statement identity;
    Statement removeContent;
    Statement insertContent;
    if (!identity.Prepare(db_, "SELECT id FROM file_index WHERE path=?;") ||
        !removeContent.Prepare(
            db_, "DELETE FROM file_content_fts WHERE rowid=?;") ||
        !insertContent.Prepare(
            db_, "INSERT INTO file_content_fts(rowid, content) VALUES (?, ?);")) {
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
      BindText(stmt.get(), 8, entry.root);
      sqlite3_bind_int(stmt.get(), 9, entry.contentState);
      sqlite3_bind_int64(stmt.get(), 10, entry.contentBytes);
      sqlite3_bind_int64(stmt.get(), 11, entry.scanGeneration);
      if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return false;
      }
      sqlite3_reset(identity.get());
      sqlite3_clear_bindings(identity.get());
      BindText(identity.get(), 1, entry.path);
      if (sqlite3_step(identity.get()) != SQLITE_ROW) {
        Exec("ROLLBACK;");
        return false;
      }
      const auto rowId = sqlite3_column_int64(identity.get(), 0);
      sqlite3_reset(removeContent.get());
      sqlite3_clear_bindings(removeContent.get());
      sqlite3_bind_int64(removeContent.get(), 1, rowId);
      if (sqlite3_step(removeContent.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return false;
      }
      if (!entry.contentText.empty()) {
        sqlite3_reset(insertContent.get());
        sqlite3_clear_bindings(insertContent.get());
        sqlite3_bind_int64(insertContent.get(), 1, rowId);
        BindText(insertContent.get(), 2, entry.contentText);
        if (sqlite3_step(insertContent.get()) != SQLITE_DONE) {
          Exec("ROLLBACK;");
          return false;
        }
      }
    }

    if (!entries.empty()) {
      Statement cleanup;
      if (!cleanup.Prepare(db_, "DELETE FROM file_index WHERE indexed_at <> ?;")) {
        Exec("ROLLBACK;");
        return false;
      }
      sqlite3_bind_int64(cleanup.get(), 1, entries.front().indexedAt);
      if (sqlite3_step(cleanup.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return false;
      }
      if (!Exec("DELETE FROM file_content_fts WHERE rowid NOT IN (SELECT id FROM file_index);")) {
        Exec("ROLLBACK;");
        return false;
      }
    } else if (!Exec("DELETE FROM file_index;")) {
      Exec("ROLLBACK;");
      return false;
    } else if (!Exec("DELETE FROM file_content_fts;")) {
      Exec("ROLLBACK;");
      return false;
    }

    if (!Exec("COMMIT;")) {
      Exec("ROLLBACK;");
      return false;
    }
    return true;
  }

  bool ReplaceFileIndex(const std::vector<FileIndexEntry>& entries) {
    return UpdateFileIndex(entries);
  }

  bool ClearFileIndex() {
    if (!Exec("BEGIN IMMEDIATE;")) return false;
    if (!Exec("DELETE FROM file_content_fts;") ||
        !Exec("DELETE FROM file_index;") || !Exec("COMMIT;")) {
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
                      "SELECT id, text, preview, captured_at, encrypted FROM clipboard_history "
                      "ORDER BY captured_at DESC, id DESC LIMIT ?;")) {
      return out;
    }
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(limit));

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      ClipboardEntry entry;
      entry.id = sqlite3_column_int64(stmt.get(), 0);
      const std::wstring storedText = ColumnText(stmt.get(), 1);
      const std::wstring storedPreview = ColumnText(stmt.get(), 2);
      const bool encrypted = sqlite3_column_int(stmt.get(), 4) != 0;
      if (encrypted) {
        const auto text = UnprotectText(storedText);
        const auto preview = UnprotectText(storedPreview);
        if (!text || !preview) continue;
        entry.text = *text;
        entry.preview = *preview;
      } else {
        entry.text = storedText;
        entry.preview = storedPreview;
      }
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
    const auto protectedText = ProtectText(text);
    const auto protectedPreview = ProtectText(preview);
    const auto contentHash = ContentHash(text);
    if (!protectedText || !protectedPreview || !contentHash) {
      SetError(SQLITE_ERROR, "failed to protect clipboard data");
      return std::nullopt;
    }
    if (!Exec("BEGIN IMMEDIATE;")) return std::nullopt;

    {
      Statement remove;
      if (!remove.Prepare(db_, "DELETE FROM clipboard_history WHERE content_hash = ?;")) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
      BindText(remove.get(), 1, *contentHash);
      if (sqlite3_step(remove.get()) != SQLITE_DONE) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
    }

    {
      Statement insert;
      if (!insert.Prepare(db_,
                          "INSERT INTO clipboard_history "
                          "(text, preview, captured_at, content_hash, encrypted) "
                          "VALUES (?, ?, ?, ?, 1);")) {
        Exec("ROLLBACK;");
        return std::nullopt;
      }
      BindText(insert.get(), 1, *protectedText);
      BindText(insert.get(), 2, *protectedPreview);
      sqlite3_bind_int64(insert.get(), 3, capturedAt);
      BindText(insert.get(), 4, *contentHash);
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

  bool PruneClipboardHistory(size_t limit) {
    Statement prune;
    if (!db_ ||
        !prune.Prepare(db_,
                       "DELETE FROM clipboard_history WHERE id NOT IN ("
                       "SELECT id FROM clipboard_history "
                       "ORDER BY captured_at DESC, id DESC LIMIT ?"
                       ");")) {
      return false;
    }
    sqlite3_bind_int64(prune.get(), 1, static_cast<sqlite3_int64>(limit));
    return sqlite3_step(prune.get()) == SQLITE_DONE;
  }

 private:
  bool IntegrityCheck() {
    Statement stmt;
    if (!stmt.Prepare(db_, "PRAGMA quick_check(1);")) {
      CaptureError();
      return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
      CaptureError();
      return false;
    }
    const auto* result =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    if (!result || std::string_view(result) != "ok") {
      SetError(SQLITE_CORRUPT, result ? result : "database integrity check failed");
      return false;
    }
    return true;
  }

  int ReadSchemaVersion() {
    Statement stmt;
    if (!stmt.Prepare(db_, "PRAGMA user_version;")) {
      CaptureError();
      return -1;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
      CaptureError();
      return -1;
    }
    return sqlite3_column_int(stmt.get(), 0);
  }

  bool HasTable(const char* name) {
    Statement stmt;
    if (!stmt.Prepare(
            db_,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;")) {
      CaptureError();
      return false;
    }
    sqlite3_bind_text(stmt.get(), 1, name, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
  }

  bool BackupBeforeMigration(const std::filesystem::path& path,
                             int targetVersion) {
    auto backupPath = path;
    backupPath += L".pre-v" + std::to_wstring(targetVersion) + L".bak";
    std::error_code ec;
    if (std::filesystem::exists(backupPath, ec)) return true;

    sqlite3* destination = nullptr;
    if (sqlite3_open16(backupPath.c_str(), &destination) != SQLITE_OK) {
      if (destination) sqlite3_close(destination);
      SetError(SQLITE_CANTOPEN, "could not create the pre-migration database backup");
      return false;
    }
    sqlite3_backup* backup =
        sqlite3_backup_init(destination, "main", db_, "main");
    if (!backup) {
      sqlite3_close(destination);
      SetError(SQLITE_ERROR, "could not initialize the pre-migration database backup");
      return false;
    }
    const int step = sqlite3_backup_step(backup, -1);
    const int finish = sqlite3_backup_finish(backup);
    const int destinationError = sqlite3_errcode(destination);
    sqlite3_close(destination);
    if ((step != SQLITE_DONE && step != SQLITE_OK) || finish != SQLITE_OK ||
        destinationError != SQLITE_OK) {
      std::filesystem::remove(backupPath, ec);
      SetError(SQLITE_ERROR, "could not complete the pre-migration database backup");
      return false;
    }
    return true;
  }

  bool MigrateSchema(int version) {
    if (version < 2) {
      if (!Exec("ALTER TABLE clipboard_history "
                "ADD COLUMN content_hash TEXT NOT NULL DEFAULT '';") ||
          !Exec("ALTER TABLE clipboard_history "
                "ADD COLUMN encrypted INTEGER NOT NULL DEFAULT 0;")) {
        return false;
      }

      std::vector<std::tuple<long long, std::wstring, std::wstring>> rows;
      Statement read;
      if (!read.Prepare(db_,
                        "SELECT id, text, preview FROM clipboard_history "
                        "WHERE encrypted=0;")) {
        CaptureError();
        return false;
      }
      while (sqlite3_step(read.get()) == SQLITE_ROW) {
        rows.emplace_back(sqlite3_column_int64(read.get(), 0),
                          ColumnText(read.get(), 1), ColumnText(read.get(), 2));
      }

      Statement update;
      if (!update.Prepare(
              db_,
              "UPDATE clipboard_history SET text=?, preview=?, content_hash=?, "
              "encrypted=1 WHERE id=?;")) {
        CaptureError();
        return false;
      }
      for (const auto& [id, text, preview] : rows) {
        const auto protectedText = ProtectText(text);
        const auto protectedPreview = ProtectText(preview);
        const auto contentHash = ContentHash(text);
        if (!protectedText || !protectedPreview || !contentHash) {
          SetError(SQLITE_ERROR, "failed to protect clipboard data during migration");
          return false;
        }
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        BindText(update.get(), 1, *protectedText);
        BindText(update.get(), 2, *protectedPreview);
        BindText(update.get(), 3, *contentHash);
        sqlite3_bind_int64(update.get(), 4, id);
        if (sqlite3_step(update.get()) != SQLITE_DONE) {
          CaptureError();
          return false;
        }
      }
    }

    if (version < 3) {
      if (!Exec("ALTER TABLE file_index RENAME TO file_index_v2;") ||
          !Exec("CREATE TABLE file_index ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "path TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                "name TEXT NOT NULL,"
                "is_directory INTEGER NOT NULL,"
                "icon_key TEXT NOT NULL,"
                "last_write_time INTEGER NOT NULL,"
                "size INTEGER NOT NULL,"
                "indexed_at INTEGER NOT NULL,"
                "root TEXT NOT NULL DEFAULT '',"
                "content_state INTEGER NOT NULL DEFAULT 0,"
                "content_bytes INTEGER NOT NULL DEFAULT 0,"
                "scan_generation INTEGER NOT NULL DEFAULT 0"
                ");") ||
          !Exec("INSERT INTO file_index "
                "(path,name,is_directory,icon_key,last_write_time,size,indexed_at) "
                "SELECT path,name,is_directory,icon_key,last_write_time,size,indexed_at "
                "FROM file_index_v2;") ||
          !Exec("DROP TABLE file_index_v2;")) {
        return false;
      }
    }
    return true;
  }

  static std::optional<std::wstring> ProtectText(const std::wstring& value) {
    DATA_BLOB input{
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)),
        reinterpret_cast<BYTE*>(const_cast<wchar_t*>(value.c_str()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"FeatherCast clipboard", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
      return std::nullopt;
    }

    DWORD chars = 0;
    if (!CryptBinaryToStringW(output.pbData, output.cbData,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &chars)) {
      LocalFree(output.pbData);
      return std::nullopt;
    }
    std::wstring encoded(chars, L'\0');
    const bool converted =
        CryptBinaryToStringW(output.pbData, output.cbData,
                             CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             encoded.data(), &chars) != FALSE;
    LocalFree(output.pbData);
    if (!converted) return std::nullopt;
    if (!encoded.empty() && encoded.back() == L'\0') encoded.pop_back();
    return encoded;
  }

  static std::optional<std::wstring> UnprotectText(const std::wstring& value) {
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(value.c_str(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64, nullptr, &bytes, nullptr,
                              nullptr)) {
      return std::nullopt;
    }
    std::vector<BYTE> encrypted(bytes);
    if (!CryptStringToBinaryW(value.c_str(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64, encrypted.data(), &bytes,
                              nullptr, nullptr)) {
      return std::nullopt;
    }
    DATA_BLOB input{bytes, encrypted.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
      return std::nullopt;
    }
    if (output.cbData < sizeof(wchar_t) ||
        output.cbData % sizeof(wchar_t) != 0) {
      LocalFree(output.pbData);
      return std::nullopt;
    }
    const auto* text = reinterpret_cast<const wchar_t*>(output.pbData);
    const size_t chars = output.cbData / sizeof(wchar_t);
    const size_t length = chars > 0 && text[chars - 1] == L'\0' ? chars - 1 : chars;
    std::wstring decoded(text, text + length);
    LocalFree(output.pbData);
    return decoded;
  }

  static std::optional<std::wstring> ContentHash(const std::wstring& value) {
    BYTE hash[32]{};
    DWORD size = static_cast<DWORD>(sizeof(hash));
    if (!CryptHashCertificate(
            0, CALG_SHA_256, 0,
            reinterpret_cast<const BYTE*>(value.data()),
            static_cast<DWORD>(value.size() * sizeof(wchar_t)), hash, &size) ||
        size != sizeof(hash)) {
      return std::nullopt;
    }
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(64);
    for (const BYTE byte : hash) {
      out.push_back(kHex[(byte >> 4) & 0x0F]);
      out.push_back(kHex[byte & 0x0F]);
    }
    return out;
  }

  bool Exec(const char* sql) const {
    if (!db_) return false;
    const int result = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) CaptureError(result);
    return result == SQLITE_OK;
  }

  void CaptureError(int fallback = SQLITE_ERROR) const {
    const int code = db_ ? sqlite3_errcode(db_) : fallback;
    const char* message = db_ ? sqlite3_errmsg(db_) : "SQLite database is not open";
    SetError(code == SQLITE_OK ? fallback : code, message ? message : "SQLite error");
  }

  void SetError(int code, std::string message) const {
    lastError_.code = code;
    lastError_.message = std::move(message);
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
  mutable StorageError lastError_;
  bool recoveredFromCorruption_ = false;
  std::filesystem::path quarantinedPath_;
};

}  // namespace feathercast::storage
