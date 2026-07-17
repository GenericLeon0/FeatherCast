#include "persistence_service.hpp"

#include <exception>
#include <utility>

namespace feathercast::persistence {

PersistenceService::PersistenceService(std::filesystem::path settingsPath,
                                       std::filesystem::path databasePath,
                                       EventSink eventSink)
    : settingsPath_(std::move(settingsPath)),
      databasePath_(std::move(databasePath)),
      eventSink_(std::move(eventSink)) {}

PersistenceService::~PersistenceService() {
  Stop(true);
}

settings_io::LoadResult PersistenceService::LoadSettingsForStartup() const {
  return settings_io::LoadSettingsFile(settingsPath_);
}

bool PersistenceService::SaveSettingsForStartup(
    const settings::Settings& settings, std::wstring* error) const {
  return settings_io::SaveSettingsFile(settingsPath_, settings, error);
}

StorageStartupState PersistenceService::LoadStorageForStartup(
    std::size_t fileLimit, std::size_t clipboardLimit) {
  StorageStartupState state;
  state.opened = EnsureStorageOpen();
  if (!state.opened) {
    state.error = storage_.LastError();
    return state;
  }

  state.recoveredFromCorruption = storage_.RecoveredFromCorruption();
  state.quarantinedPath = storage_.QuarantinedPath();
  state.files = storage_.LoadFileIndex(fileLimit);
  state.clipboard = storage_.LoadClipboardHistory(clipboardLimit);
  return state;
}

void PersistenceService::Start() {
  executor_.Start(1, [this](std::exception_ptr) {
    EmitWorkerFailure(L"persistence");
  });
}

void PersistenceService::Stop(bool drainPending) {
  executor_.Shutdown(drainPending);
  storage_.Close();
  std::lock_guard lock(settingsMutex_);
  pendingSettings_.reset();
  settingsSaveScheduled_ = false;
}

bool PersistenceService::SaveSettings(settings::Settings settings) {
  bool schedule = false;
  {
    std::lock_guard lock(settingsMutex_);
    pendingSettings_ = std::move(settings);
    if (!settingsSaveScheduled_) {
      settingsSaveScheduled_ = true;
      schedule = true;
    }
  }
  if (!schedule) return true;
  if (executor_.Submit(
          [this](std::stop_token token) { SettingsSaveLoop(token); })) {
    return true;
  }

  {
    std::lock_guard lock(settingsMutex_);
    pendingSettings_.reset();
    settingsSaveScheduled_ = false;
  }
  return false;
}

bool PersistenceService::PruneClipboard(std::size_t limit) {
  return executor_.Submit([this, limit](std::stop_token token) {
    if (token.stop_requested()) return;
    const bool succeeded =
        EnsureStorageOpen() && storage_.PruneClipboardHistory(limit);
    Emit(ClipboardPruned{
        succeeded, succeeded ? storage::StorageError{} : storage_.LastError()});
  });
}

bool PersistenceService::ReplaceFileIndex(
    std::vector<storage::FileIndexEntry> entries) {
  return executor_.Submit(
      [this, entries = std::move(entries)](std::stop_token token) {
        if (token.stop_requested()) return;
        const bool succeeded =
            EnsureStorageOpen() && storage_.ReplaceFileIndex(entries);
        Emit(FileIndexWriteCompleted{
            succeeded,
            succeeded ? storage::StorageError{} : storage_.LastError()});
      });
}

bool PersistenceService::UpdateFileIndex(
    std::vector<storage::FileIndexEntry> entries) {
  return executor_.Submit(
      [this, entries = std::move(entries)](std::stop_token token) {
        if (token.stop_requested()) return;
        const bool succeeded =
            EnsureStorageOpen() && storage_.UpdateFileIndex(entries);
        Emit(FileIndexWriteCompleted{
            succeeded,
            succeeded ? storage::StorageError{} : storage_.LastError()});
      });
}

bool PersistenceService::StoreClipboard(std::wstring text,
                                        std::wstring preview,
                                        long long capturedAt,
                                        std::size_t limit) {
  return executor_.Submit(
      [this, text = std::move(text), preview = std::move(preview),
       capturedAt, limit](std::stop_token token) {
        if (token.stop_requested()) return;
        std::optional<storage::ClipboardEntry> entry;
        if (EnsureStorageOpen()) {
          entry = storage_.AddClipboardEntry(text, preview, capturedAt, limit);
        }
        Emit(ClipboardStored{
            std::move(entry),
            entry ? storage::StorageError{} : storage_.LastError()});
      });
}

bool PersistenceService::LoadClipboard(std::size_t limit) {
  return executor_.Submit([this, limit](std::stop_token token) {
    if (token.stop_requested()) return;
    std::vector<storage::ClipboardEntry> entries;
    if (EnsureStorageOpen()) {
      entries = storage_.LoadClipboardHistory(limit);
    }
    Emit(ClipboardLoaded{
        std::move(entries),
        storage_.IsOpen() ? storage::StorageError{} : storage_.LastError()});
  });
}

bool PersistenceService::Clear(app::StorageOperationKind kind) {
  return executor_.Submit([this, kind](std::stop_token token) {
    if (token.stop_requested()) return;
    bool succeeded = false;
    if (EnsureStorageOpen()) {
      succeeded = kind == app::StorageOperationKind::ClearClipboard
                      ? storage_.ClearClipboardHistory()
                      : storage_.ClearFileIndex();
    }
    Emit(StorageClearCompleted{
        kind, succeeded,
        succeeded ? storage::StorageError{} : storage_.LastError()});
  });
}

bool PersistenceService::EnsureStorageOpen() {
  return storage_.IsOpen() || storage_.Open(databasePath_);
}

void PersistenceService::Emit(Event event) const {
  if (eventSink_) eventSink_(std::move(event));
}

void PersistenceService::EmitWorkerFailure(
    const std::wstring& operation) const {
  Emit(WorkerFailed{operation});
}

void PersistenceService::SettingsSaveLoop(std::stop_token stopToken) {
  for (;;) {
    std::optional<settings::Settings> pending;
    {
      std::lock_guard lock(settingsMutex_);
      if (!pendingSettings_ || stopToken.stop_requested()) {
        settingsSaveScheduled_ = false;
        return;
      }
      pending = std::move(pendingSettings_);
      pendingSettings_.reset();
    }

    std::wstring error;
    const bool succeeded =
        settings_io::SaveSettingsFile(settingsPath_, *pending, &error);
    Emit(SettingsSaveCompleted{succeeded, std::move(error)});

    std::lock_guard lock(settingsMutex_);
    if (!pendingSettings_) {
      settingsSaveScheduled_ = false;
      return;
    }
  }
}

}  // namespace feathercast::persistence
