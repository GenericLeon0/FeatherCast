#include "persistence_service.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace {

std::filesystem::path TestRoot() {
  wchar_t path[MAX_PATH]{};
  assert(GetTempPathW(MAX_PATH, path) > 0);
  return std::filesystem::path(path) /
         (L"FeatherCastPersistenceServiceTests-" +
          std::to_wstring(GetCurrentProcessId()));
}

}  // namespace

int main() {
  namespace persistence = feathercast::persistence;

  const auto root = TestRoot();
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root);

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<persistence::Event> events;
  persistence::PersistenceService service(
      root / L"settings.json", root / L"feathercast.db",
      [&](persistence::Event event) {
        {
          std::lock_guard lock(mutex);
          events.push_back(std::move(event));
        }
        cv.notify_all();
      });

  const auto missing = service.LoadSettingsForStartup();
  assert(missing.status == feathercast::settings::ParseStatus::Missing);
  const auto startup = service.LoadStorageForStartup(100, 20);
  assert(startup.opened);

  service.Start();
  feathercast::settings::Settings settings;
  settings.shortcut = L"Ctrl+Shift+Space";
  assert(service.SaveSettings(settings));
  assert(service.StoreClipboard(L"secret", L"secret", 1, 20));
  assert(service.Clear(feathercast::app::StorageOperationKind::ClearClipboard));

  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(5), [&] {
      bool saved = false;
      bool stored = false;
      bool cleared = false;
      for (const auto& event : events) {
        saved = saved ||
                (std::holds_alternative<persistence::SettingsSaveCompleted>(
                     event) &&
                 std::get<persistence::SettingsSaveCompleted>(event)
                     .succeeded);
        stored = stored ||
                 (std::holds_alternative<persistence::ClipboardStored>(event) &&
                  std::get<persistence::ClipboardStored>(event).entry
                      .has_value());
        cleared =
            cleared ||
            (std::holds_alternative<persistence::StorageClearCompleted>(
                 event) &&
             std::get<persistence::StorageClearCompleted>(event).succeeded);
      }
      return saved && stored && cleared;
    }));
  }

  service.Stop(true);
  const auto loaded =
      feathercast::settings_io::LoadSettingsFile(root / L"settings.json");
  assert(loaded.status == feathercast::settings::ParseStatus::Valid);
  assert(loaded.value.shortcut == L"Ctrl+Shift+Space");

  std::filesystem::remove_all(root, ec);
  assert(!ec);
  return 0;
}
