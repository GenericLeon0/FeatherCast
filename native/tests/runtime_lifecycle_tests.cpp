#include "background_services.hpp"
#include "discovery_service.hpp"
#include "persistence_service.hpp"
#include "search_coordinator.hpp"
#include "test_framework.hpp"
#include "ui_event_queue.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <variant>

namespace {

constexpr UINT kEventsReady = WM_APP + 42;
using LifecycleEvent =
    std::variant<int, std::wstring, feathercast::app::DiscoveryResult,
                 feathercast::app::ResultsCollection,
                 feathercast::persistence::Event>;

}  // namespace

int main() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t className[] = L"FeatherCastLifecycleTestWindow";
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = DefWindowProcW;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  assert(RegisterClassW(&windowClass) != 0);
  HWND window = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, nullptr);
  assert(window != nullptr);

  feathercast::runtime::UiEventQueue<LifecycleEvent> events([&] {
    PostMessageW(window, kEventsReady, 0, 0);
  });
  const auto root = std::filesystem::temp_directory_path() /
                    (L"FeatherCast-lifecycle-" +
                     std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);

  feathercast::persistence::PersistenceService persistence(
      root / L"settings.json", root / L"feathercast.db",
      [&](feathercast::persistence::Event event) {
        events.Push(std::move(event));
      });
  persistence.Start();
  assert(persistence.SaveSettings({}));

  feathercast::search::SearchCoordinator search(
      [&](feathercast::app::ResultsCollection result) {
        events.Push(std::move(result));
      });
  search.Start([](const feathercast::app::QueryRequest& request) {
    feathercast::app::ResultsCollection result;
    result.generation = request.generation;
    return result;
  });
  feathercast::app::QueryRequest query;
  query.generation = 1;
  assert(search.Query(std::move(query)));

  feathercast::discovery_runtime::DiscoveryService discovery(
      [&](feathercast::app::DiscoveryResult result) {
        events.Push(std::move(result));
      });
  discovery.Start([](const feathercast::app::DiscoveryRequest& request,
                     std::stop_token) {
    feathercast::app::DiscoveryResult result;
    result.generation = request.generation;
    return std::optional{std::move(result)};
  });
  feathercast::app::DiscoveryRequest refresh;
  refresh.generation = 1;
  assert(discovery.Refresh(std::move(refresh)));

  feathercast::runtime::LaunchService launch;
  launch.Start(1);
  assert(launch.Submit([&](std::stop_token) { events.Push(1); }));

  feathercast::runtime::IconResolver icons(
      [&](feathercast::runtime::DecodedIcon icon) {
        events.Push(std::move(icon.key));
      });
  icons.Start(1, [](const std::wstring& key, std::stop_token) {
    feathercast::runtime::DecodedIcon icon;
    icon.key = key;
    icon.width = 1;
    icon.height = 1;
    icon.stride = 4;
    icon.pixels.resize(4);
    return std::optional{std::move(icon)};
  });
  assert(icons.Queue(L"fake-icon"));

  std::size_t delivered = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
  while (delivered < 5 && std::chrono::steady_clock::now() < deadline) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == kEventsReady) {
        delivered += events.Drain().size();
      } else {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    std::this_thread::yield();
  }
  assert(delivered >= 5);

  icons.Stop();
  launch.Stop();
  discovery.Stop();
  search.Stop();
  persistence.Stop(true);
  events.Close();
  assert(!events.Push(9));
  DestroyWindow(window);
  UnregisterClassW(className, instance);
  std::filesystem::remove_all(root, ec);
  return 0;
}
