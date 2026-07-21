#include "calculator.hpp"
#include "accessibility.hpp"
#include "app_types.hpp"
#include "audio_volume.hpp"
#include "background_executor.hpp"
#include "background_services.hpp"
#include "capability_catalog.hpp"
#include "command_catalog.hpp"
#include "converter.hpp"
#include "core.hpp"
#include "discovery.hpp"
#include "discovery_service.hpp"
#include "emoji.hpp"
#include "extension_manager.hpp"
#include "file_index_service.hpp"
#include "file_search_service.hpp"
#include "game_discovery.hpp"
#include "interaction_state.hpp"
#include "json.hpp"
#include "library.hpp"
#include "library_ui.hpp"
#include "motion.hpp"
#include "network_client.hpp"
#include "persistence_service.hpp"
#include "preview_service.hpp"
#include "run_command.hpp"
#include "search_coordinator.hpp"
#include "search_pipeline.hpp"
#include "settings.hpp"
#include "settings_catalog.hpp"
#include "settings_io.hpp"
#include "shortcut.hpp"
#include "snippets.hpp"
#include "snippets_io.hpp"
#include "storage.hpp"
#include "symbols.hpp"
#include "system_settings.hpp"
#include "theme.hpp"
#include "text_edit.hpp"
#include "updater.hpp"
#include "uuid_utilities.hpp"
#include "ui_event_queue.hpp"
#include "ui_renderer.hpp"
#include "result_icons.hpp"
#include "ui_state.hpp"
#include "version.hpp"
#include "window_layout.hpp"

#include <windows.h>
#include <malloc.h>
#include <cmath>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <imm.h>
#include <dxgi1_2.h>
#include <knownfolders.h>
#include <powrprof.h>
#include <propidl.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using feathercast::shortcut::GenericModifier;
using feathercast::shortcut::IsExclusiveWinShortcut;
using feathercast::shortcut::IsModifier;
using feathercast::shortcut::ModifierPressed;
using feathercast::shortcut::ParseShortcut;
using feathercast::shortcut::PressedModifiers;
using feathercast::shortcut::ShortcutRecorder;
using feathercast::shortcut::ShortcutRuntime;
using feathercast::ui::NavigationState;
using feathercast::ui::ResultIcon;
using feathercast::shortcut::ShortcutSpec;
using feathercast::shortcut::ShouldHandleInLowLevelHook;
using feathercast::shortcut::ToHotKeySpec;
using namespace feathercast::app;

namespace {

constexpr int IDI_APP_ICON = 101;
constexpr wchar_t kWindowClass[] = L"FeatherCastNativeWindow";
constexpr wchar_t kSettingsWindowClass[] = L"FeatherCastSettingsWindow";
constexpr wchar_t kVolumeWindowClass[] = L"FeatherCastVolumeWindow";
constexpr wchar_t kMutexName[] = L"FeatherCastNativeSingleInstance";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SHOW_SEARCH = WM_APP + 2;
constexpr UINT WM_REBUILD_RESULTS = WM_APP + 4;
constexpr UINT WM_SHORTCUT_TOGGLE = WM_APP + 6;
constexpr UINT WM_TRACK_RECENT = WM_APP + 8;
constexpr UINT WM_ANIMATION_FRAME = WM_APP + 9;
constexpr UINT WM_EXTENSION_ACTIVATED = WM_APP + 10;
constexpr UINT WM_EXTENSION_RELOAD_READY = WM_APP + 15;
constexpr UINT WM_BACKGROUND_TASK_ERROR = WM_APP + 17;
constexpr UINT WM_APP_EVENTS = WM_APP + 18;
constexpr UINT WM_RENDER_RECOVER = WM_APP + 19;
constexpr UINT WM_OVERLAY_RETRY_ACTIVATION = WM_APP + 20;
constexpr int HOTKEY_OPEN_SEARCH = 0x4C43;
constexpr int HOTKEY_VALIDATE_SHORTCUT = 0x4C44;
constexpr UINT TIMER_MEM_TRIM = 3;
constexpr UINT TIMER_VOLUME_REFRESH = 5;
constexpr UINT TIMER_OVERLAY_ACTIVATE = 6;
constexpr UINT TIMER_PREVIEW_LOAD = 7;
constexpr UINT TIMER_FILE_SNAPSHOT = 8;
constexpr UINT TIMER_SHORTCUT_TOGGLE = 9;
constexpr UINT OVERLAY_ACTIVATION_INTERVAL_MS = 50;

enum class OverlayCloseReason {
  ExplicitDismiss,
  PassiveFocusLoss,
  Action,
  Transition,
};

struct ForegroundSnapshot {
  HWND hwnd = nullptr;
  DWORD processId = 0;
  DWORD threadId = 0;
  DWORD inputTime = 0;
  ULONGLONG observedTick = 0;
};

struct RestoreCandidate : ForegroundSnapshot {
  std::uint64_t generation = 0;
};

struct PendingOverlayClose {
  std::optional<RestoreCandidate> candidate;
  std::uint64_t generation = 0;
  OverlayCloseReason reason = OverlayCloseReason::Action;
};

struct ShortcutToggleRequest {
  std::uint64_t id = 0;
  std::optional<ForegroundSnapshot> foreground;
  bool deferUntilWinRelease = false;
};

constexpr int WIN_WIDTH = 720;
constexpr int WIN_HEIGHT = 470;
constexpr int COMPACT_BASE_HEIGHT = 60;
constexpr int RECENT_LIMIT = 8;
constexpr int MAX_RESULTS = 200;
constexpr int MIN_OVERLAY_WIDTH = 560;
constexpr int MAX_OVERLAY_WIDTH = 980;
constexpr int SETTINGS_WIDTH = 760;
constexpr int SETTINGS_HEIGHT = 560;
constexpr int VOLUME_WIDTH = 440;
constexpr int VOLUME_HEIGHT = 170;
constexpr int MIN_RESULTS = 25;
constexpr int MAX_RESULT_SETTING = 400;
constexpr int MIN_CLIPBOARD_HISTORY_LIMIT = 1;
constexpr int MAX_CLIPBOARD_HISTORY_LIMIT = 500;
constexpr int MIN_FILE_INDEX_ENTRIES = 100;
constexpr int MAX_FILE_INDEX_ENTRIES = 100000;
constexpr size_t CLIPBOARD_TEXT_CAP_CHARS = (16 * 1024) / sizeof(wchar_t);

// --- RAII helpers for raw Win32 resources ------------------------------------
// Stateless deleters keep the smart pointers zero-overhead; each guards a handle
// or allocation so it is released on every return/exception path.
struct IconDeleter {
  void operator()(HICON icon) const { if (icon) DestroyIcon(icon); }
};
struct GdiObjectDeleter {
  void operator()(void* obj) const { if (obj) DeleteObject(obj); }
};
struct DcDeleter {
  void operator()(HDC dc) const { if (dc) DeleteDC(dc); }
};
struct HandleDeleter {
  void operator()(HANDLE handle) const {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
struct CoTaskMemDeleter {
  void operator()(void* memory) const { if (memory) CoTaskMemFree(memory); }
};

using UniqueIcon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;
using UniqueBitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, GdiObjectDeleter>;
using UniqueDC = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;
template <typename T>
using CoMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

// Owns the screen device context returned by GetDC(nullptr) (released, not deleted).
struct ScreenDC {
  HDC dc = GetDC(nullptr);
  ~ScreenDC() { if (dc) ReleaseDC(nullptr, dc); }
  HDC get() const { return dc; }
  ScreenDC() = default;
  ScreenDC(const ScreenDC&) = delete;
  ScreenDC& operator=(const ScreenDC&) = delete;
};

// Generic scope guard for acquire/release pairs that do not fit a unique_ptr
// (CloseClipboard, CoTaskMemFree on strict-typed PIDLs, conditional GlobalFree).
template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
  ~ScopeExit() { if (active_) fn_(); }
  void Release() { active_ = false; }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

 private:
  F fn_;
  bool active_ = true;
};

// Settings model and JSON round-trip live in settings.hpp (unit-tested there).
using Quicklink = feathercast::settings::Quicklink;
using Settings = feathercast::settings::Settings;
using AnimationLevel = feathercast::settings::AnimationLevel;
using AnimationKind = feathercast::settings::AnimationKind;
using feathercast::settings::JsonEscape;

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return L"";
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(needed, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
  return out;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return "";
  const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string out(needed, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

using feathercast::core::Lower;
using feathercast::core::Trim;
using feathercast::discovery::BaseNameNoExt;
using feathercast::discovery::CleanName;
using feathercast::discovery::IsSystemEssentialName;
using feathercast::discovery::KeywordsFor;
using feathercast::discovery::NameKey;
using feathercast::discovery::ShouldSkipName;
using feathercast::discovery::StartsWith;
using feathercast::discovery::UniqueKeywords;

// The user's locale currency as an ISO-4217 code (e.g. "EUR"), used as the
// default conversion target for queries like "USD 5". Falls back to USD.
std::wstring DetectLocaleCurrency() {
  wchar_t buffer[16] = {};
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SINTLSYMBOL, buffer,
                      static_cast<int>(std::size(buffer))) > 0) {
    std::wstring code = Trim(buffer);
    if (code.size() >= 3) {
      code = code.substr(0, 3);
      for (auto& ch : code) ch = static_cast<wchar_t>(std::towupper(ch));
      return code;
    }
  }
  return L"USD";
}

long long UnixNow() {
  return static_cast<long long>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::wstring PrimaryAppId(const AppEntry& app) {
  if (!app.id.empty()) return app.id;
  if (!app.path.empty()) return app.path;
  return app.launchTarget;
}

std::vector<std::wstring> AppKeys(const AppEntry& app) {
  std::vector<std::wstring> keys;
  auto add = [&](const std::wstring& key) {
    if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
  };
  add(app.id);
  add(app.path);
  add(app.launchTarget);
  add(app.targetPath);
  return keys;
}

AppEntry FileIndexApp(const feathercast::storage::FileIndexEntry& entry) {
  AppEntry app;
  app.id = L"file:" + entry.path;
  app.name = entry.name;
  app.path = entry.path;
  app.source = L"file";
  app.launchType = LaunchType::Exe;
  app.launchTarget = entry.path;
  app.iconKey = entry.iconKey.empty() ? entry.path : entry.iconKey;
  app.fileIsDirectory = entry.isDirectory;
  app.fileLastWriteTime = entry.lastWriteTime;
  app.fileSize = entry.size;
  app.fileIndexedAt = entry.indexedAt;
  return app;
}

feathercast::storage::FileIndexEntry StorageFileEntry(const AppEntry& app) {
  return {
    app.path,
    app.name,
    app.fileIsDirectory,
    app.iconKey,
    app.fileLastWriteTime,
    app.fileSize,
    app.fileIndexedAt,
  };
}

bool ContainsValue(const std::vector<std::wstring>& values, const std::wstring& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool ContainsAnyAppKey(const std::vector<std::wstring>& values, const AppEntry& app) {
  for (const auto& key : AppKeys(app)) {
    if (ContainsValue(values, key)) return true;
  }
  return false;
}

void RemoveValue(std::vector<std::wstring>& values, const std::wstring& value) {
  values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

bool IsStoreLikeSource(const std::wstring& source) {
  return source == L"appx" || source == L"start" || source == L"alias";
}

DisplayItem AppDisplay(const AppEntry& app) {
  DisplayItem item;
  item.app = app;
  return item;
}

// Builds a synthetic app entry for a user quicklink so it flows through the
// normal search pool, ranking, and ShellExecute launch path.
AppEntry QuicklinkApp(const Quicklink& link) {
  AppEntry app;
  app.id = L"quicklink:" + link.keyword;
  app.name = link.name.empty() ? link.keyword : link.name;
  app.source = L"quicklink";
  app.launchType = LaunchType::Exe;
  app.launchTarget = link.target;
  app.keywords = {link.keyword};
  return app;
}

DisplayItem WindowDisplay(const WindowEntry& window) {
  DisplayItem item;
  item.isWindow = true;
  item.window = window;
  return item;
}

DisplayItem ExtensionDisplay(const feathercast::extensions::QueryResultItem& result) {
  DisplayItem item;
  item.isExtension = true;
  item.extension = result;
  item.commandDetail = result.pluginName.empty() ? L"Extension" : L"Extension - " + result.pluginName;
  if (!result.subtitle.empty()) item.commandDetail += L" - " + result.subtitle;
  item.commandKeywords = result.keywords;
  item.commandKeywords.push_back(result.title);
  item.commandKeywords.push_back(result.pluginName);
  item.commandKeywords.push_back(result.subtitle);
  return item;
}

std::wstring SingleLinePreview(std::wstring text, size_t maxChars = 96) {
  for (auto& ch : text) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
  }
  text = Trim(std::move(text));
  if (text.size() <= maxChars) return text;
  return text.substr(0, maxChars - 3) + L"...";
}

DisplayItem SnippetDisplay(const feathercast::snippets::Snippet& snippet) {
  DisplayItem item;
  item.isSnippet = true;
  item.snippet = snippet;
  item.commandDetail = L"Snippet - " + snippet.keyword;
  item.commandKeywords = {L"snippet", L"text", L"paste", snippet.keyword, snippet.text};
  return item;
}

DisplayItem ClipboardDisplay(const ClipboardEntry& entry) {
  DisplayItem item;
  item.isClipboard = true;
  item.clipboard = entry;
  item.commandDetail = L"Clipboard History";
  item.commandKeywords = {L"clipboard", L"history", L"paste", entry.text};
  return item;
}

bool PointInRect(const RectF& rect, float x, float y) {
  return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

D2D1_RECT_F ToD2D(const RectF& rect) {
  return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
    out = raw;
  }
  CoMemPtr<wchar_t> rawOwner(raw);
  return out;
}

std::wstring EnvironmentPath(const wchar_t* name) {
  wchar_t buf[32768]{};
  const DWORD size = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
  if (size == 0 || size >= std::size(buf)) return L"";
  return buf;
}

std::wstring WindowsDirectoryPath() {
  wchar_t buf[MAX_PATH]{};
  const UINT size = GetWindowsDirectoryW(buf, static_cast<UINT>(std::size(buf)));
  if (size == 0 || size >= std::size(buf)) return L"";
  return buf;
}

AppEntry SystemShellFolder(std::wstring id, std::wstring name, std::wstring target,
                           std::vector<std::wstring> keywords) {
  AppEntry app;
  app.id = L"system-folder:" + std::move(id);
  app.name = std::move(name);
  app.source = L"system-folder";
  app.launchType = LaunchType::Shell;
  app.launchTarget = std::move(target);
  app.iconKey = app.launchTarget;
  app.systemEssential = true;
  app.keywords = std::move(keywords);
  return app;
}

AppEntry SystemPathFolder(std::wstring id, std::wstring name, std::wstring path,
                          std::vector<std::wstring> keywords) {
  AppEntry app;
  app.id = L"system-folder:" + std::move(id);
  app.name = std::move(name);
  app.path = std::move(path);
  app.source = L"system-folder";
  app.launchType = LaunchType::Shell;
  app.launchTarget = app.path;
  app.iconKey = app.path;
  app.systemEssential = true;
  app.keywords = std::move(keywords);
  return app;
}

std::optional<std::wstring> GenerateUuidText() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) return std::nullopt;
  guid.Data3 = static_cast<unsigned short>((guid.Data3 & 0x0fffU) | 0x4000U);
  guid.Data4[0] = static_cast<unsigned char>((guid.Data4[0] & 0x3fU) | 0x80U);
  return feathercast::uuid_utilities::Format(guid);
}

std::vector<AppEntry> SystemFolderEntries() {
  std::vector<AppEntry> out;
  std::set<std::wstring> seen;
  auto add = [&](AppEntry app) {
    const std::wstring key = Lower(!app.path.empty() ? app.path : app.launchTarget);
    if (app.name.empty() || key.empty() || seen.contains(key)) return;
    seen.insert(key);
    out.push_back(std::move(app));
  };
  auto addKnown = [&](REFKNOWNFOLDERID folder, const std::wstring& id, const std::wstring& name,
                      std::vector<std::wstring> keywords) {
    const std::wstring path = KnownFolderPath(folder);
    if (!path.empty()) add(SystemPathFolder(id, name, path, std::move(keywords)));
  };
  auto addEnv = [&](const wchar_t* env, const std::wstring& id, const std::wstring& name,
                    std::vector<std::wstring> keywords) {
    const std::wstring path = EnvironmentPath(env);
    if (!path.empty()) add(SystemPathFolder(id, name, path, std::move(keywords)));
  };

  add(SystemShellFolder(L"this-pc", L"This PC", L"shell:MyComputerFolder", {L"computer", L"drives", L"explorer"}));
  add(SystemShellFolder(L"recycle-bin", L"Recycle Bin", L"shell:RecycleBinFolder", {L"trash", L"deleted", L"bin"}));
  add(SystemShellFolder(L"control-panel", L"Control Panel", L"shell:ControlPanelFolder", {L"settings", L"system"}));
  add(SystemShellFolder(L"network", L"Network", L"shell:NetworkPlacesFolder", {L"network places", L"shares"}));
  for (auto entry : feathercast::system_settings::Catalog()) {
    add(std::move(entry));
  }
  addKnown(FOLDERID_Profile, L"profile", L"User Profile", {L"home", L"user folder"});
  addKnown(FOLDERID_Desktop, L"desktop", L"Desktop", {L"desktop folder"});
  addKnown(FOLDERID_Documents, L"documents", L"Documents", {L"docs", L"files"});
  addKnown(FOLDERID_Downloads, L"downloads", L"Downloads", {L"downloaded files"});
  addKnown(FOLDERID_Pictures, L"pictures", L"Pictures", {L"photos", L"images"});
  addKnown(FOLDERID_Music, L"music", L"Music", {L"audio", L"songs"});
  addKnown(FOLDERID_Videos, L"videos", L"Videos", {L"movies", L"media"});
  addKnown(FOLDERID_RoamingAppData, L"roaming-appdata", L"Roaming AppData", {L"appdata", L"roaming"});
  addKnown(FOLDERID_LocalAppData, L"local-appdata", L"Local AppData", {L"appdata", L"local"});
  addKnown(FOLDERID_Startup, L"startup", L"Startup", {L"startup folder", L"login apps"});
  addKnown(FOLDERID_Programs, L"programs", L"Programs", {L"start menu", L"programs folder"});
  addEnv(L"ProgramFiles", L"program-files", L"Program Files", {L"installed apps"});
  addEnv(L"ProgramFiles(x86)", L"program-files-x86", L"Program Files (x86)", {L"installed apps", L"32 bit"});
  if (const std::wstring windows = WindowsDirectoryPath(); !windows.empty()) {
    add(SystemPathFolder(L"windows", L"Windows", windows, {L"system root", L"windows folder"}));
  }
  return out;
}

std::filesystem::path UserDataPath() {
  std::filesystem::path root = KnownFolderPath(FOLDERID_RoamingAppData);
  if (root.empty()) {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    root = buf;
  }
  root /= L"FeatherCast";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  return root;
}

std::filesystem::path LocalDataPath();

std::atomic<bool> g_diagnosticsEnabled = false;

// Opt-in diagnostics contain only state counters, durations, and error codes.
// Queries, paths, names, and user content are never written.
std::wstring ToHex(unsigned value) {
  wchar_t buf[16]{};
  swprintf_s(buf, L"%08X", value);
  return buf;
}

void DebugLaunchLog(const std::wstring& line) {
  if (!g_diagnosticsEnabled.load()) return;
  const auto path = LocalDataPath() / L"launch-debug.log";
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) > 1024 * 1024) {
    const auto rotated = LocalDataPath() / L"launch-debug.previous.log";
    std::filesystem::remove(rotated, ec);
    ec.clear();
    std::filesystem::rename(path, rotated, ec);
  }
  std::wofstream f(path, std::ios::app);
  if (!f) return;
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t ts[32]{};
  swprintf_s(ts, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  f << ts << line << L"\n";
}

void DebugFocusLog(const std::wstring& line) {
  if (!g_diagnosticsEnabled.load()) return;
  const auto path = LocalDataPath() / L"focus-debug.log";
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) > 1024 * 1024) {
    const auto rotated = LocalDataPath() / L"focus-debug.previous.log";
    std::filesystem::remove(rotated, ec);
    ec.clear();
    std::filesystem::rename(path, rotated, ec);
  }
  std::wofstream file(path, std::ios::app);
  if (!file) return;
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t timestamp[32]{};
  swprintf_s(timestamp, L"%02d:%02d:%02d.%03d ", time.wHour,
             time.wMinute, time.wSecond, time.wMilliseconds);
  file << timestamp << line << L"\n";
}

std::filesystem::path LocalDataPath() {
  std::filesystem::path root = KnownFolderPath(FOLDERID_LocalAppData);
  if (root.empty()) {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    root = buf;
  }
  root /= L"FeatherCast";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  return root;
}

void DebugGraphicsLog(const std::wstring& line) {
  if (!g_diagnosticsEnabled.load()) return;
  const auto path = LocalDataPath() / L"graphics-debug.log";
  std::wofstream file(path, std::ios::app);
  if (!file) return;
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t timestamp[32]{};
  swprintf_s(timestamp, L"%02d:%02d:%02d.%03d ", time.wHour, time.wMinute,
             time.wSecond, time.wMilliseconds);
  file << timestamp << line << L"\n";
}

bool MigrateLegacyOperationalData() {
  const auto roaming = UserDataPath();
  const auto local = LocalDataPath();
  bool foundLegacyData = false;

  auto migrateFile = [&](const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) return;
    foundLegacyData = true;
    if (std::filesystem::exists(destination, ec)) return;
    std::filesystem::rename(source, destination, ec);
    if (!ec) return;

    ec.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
    if (!ec) {
      std::error_code removeEc;
      std::filesystem::remove(source, removeEc);
    }
  };

  migrateFile(roaming / L"feathercast.db", local / L"feathercast.db");
  migrateFile(roaming / L"feathercast.db-wal", local / L"feathercast.db-wal");
  migrateFile(roaming / L"feathercast.db-shm", local / L"feathercast.db-shm");

  const auto oldIcons = roaming / L"icon-cache-native";
  const auto newIcons = local / L"icon-cache-native";
  std::error_code ec;
  if (std::filesystem::is_directory(oldIcons, ec)) {
    foundLegacyData = true;
    if (!std::filesystem::exists(newIcons, ec)) {
      std::filesystem::rename(oldIcons, newIcons, ec);
      if (ec) {
        ec.clear();
        std::filesystem::copy(oldIcons, newIcons, std::filesystem::copy_options::recursive, ec);
        if (!ec) {
          std::error_code removeEc;
          std::filesystem::remove_all(oldIcons, removeEc);
        }
      }
    }
  }
  return foundLegacyData;
}

std::filesystem::path UpdatesPath() {
  auto path = LocalDataPath() / L"updates";
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return path;
}

std::filesystem::path UpdateLogPath() {
  return LocalDataPath() / L"update-log.txt";
}

void AppendUpdateLog(const std::wstring& message) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::ofstream file(UpdateLogPath(), std::ios::binary | std::ios::app);
  if (!file) return;
  file << UnixNow() << " " << WideToUtf8(message) << "\n";
}

std::filesystem::path ExeDirectory() {
  wchar_t exePath[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  return std::filesystem::path(exePath).parent_path();
}

std::filesystem::path SettingsPath() {
  return UserDataPath() / L"settings.json";
}

std::filesystem::path SnippetsPath() {
  return UserDataPath() / L"snippets.json";
}

std::filesystem::path DatabasePath() {
  return LocalDataPath() / L"feathercast.db";
}

std::filesystem::path ThemePath() {
  return UserDataPath() / L"theme.json";
}

void EnsureSnippetsFile() {
  const auto path = SnippetsPath();
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return;
  file << "{\n  \"snippets\": [\n"
          "    {\"keyword\": \"sig\", \"name\": \"Email Signature\", \"text\": \"Best,\\nLeon\"}\n"
          "  ]\n}\n";
}

std::filesystem::path CurrencyCachePath() {
  return UserDataPath() / L"currency-rates.json";
}

// Extracts the {"rates": {"CODE": number, ...}} member as a currency-rate map.
std::map<std::wstring, double> RatesFromJson(const std::string& text) {
  std::map<std::wstring, double> out;
  const auto root = feathercast::json::Parse(text);
  if (!root) return out;
  const auto* rates = root->Find("rates");
  if (!rates || rates->type != feathercast::json::Value::Type::Object) return out;
  for (const auto& member : rates->object) {
    if (member.value.type == feathercast::json::Value::Type::Number) {
      out[Utf8ToWide(member.key)] = member.value.number;
    }
  }
  return out;
}

CurrencyRates LoadCurrencyCache() {
  CurrencyRates rates;
  std::ifstream file(CurrencyCachePath(), std::ios::binary);
  if (!file) return rates;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string json = buffer.str();
  if (const auto root = feathercast::json::Parse(json)) {
    if (const auto* fetchedAt = root->Find("fetchedAt"); fetchedAt && fetchedAt->type == feathercast::json::Value::Type::Number) {
      rates.fetchedAt = static_cast<long long>(fetchedAt->number);
    }
  }
  rates.perUsd = RatesFromJson(json);
  return rates;
}

void SaveCurrencyCache(const CurrencyRates& rates) {
  const auto path = CurrencyCachePath();
  auto temp = path;
  temp += L".tmp";
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file) return;
  file << "{\n  \"fetchedAt\": " << rates.fetchedAt << ",\n  \"rates\": {";
  bool first = true;
  for (const auto& [code, value] : rates.perUsd) {
    if (!first) file << ", ";
    first = false;
    file << "\"" << JsonEscape(code) << "\": " << value;
  }
  file << "}\n}\n";
  file.close();
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
  }
}

// Grants the current process the SE_SHUTDOWN privilege, required before
// ExitWindowsEx can shut down or reboot the machine.
bool EnableShutdownPrivilege() {
  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
    return false;
  }
  UniqueHandle token(rawToken);
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid)) {
    return false;
  }
  AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr);
  return GetLastError() == ERROR_SUCCESS;
}

bool SetStartOnStartup(bool enable) {
  HKEY hKey = nullptr;
  LSTATUS status = RegOpenKeyExW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      0,
      KEY_WRITE,
      &hKey
  );
  if (status != ERROR_SUCCESS) return false;

  if (enable) {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
      RegCloseKey(hKey);
      return false;
    }
    const std::wstring pathStr = L"\"" + std::wstring(exePath) + L"\"";
    status = RegSetValueExW(
        hKey,
        L"FeatherCast",
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(pathStr.c_str()),
        static_cast<DWORD>((pathStr.length() + 1) * sizeof(wchar_t)));
  } else {
    status = RegDeleteValueW(hKey, L"FeatherCast");
    if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
  }
  RegCloseKey(hKey);
  return status == ERROR_SUCCESS;
}

void ClampSettings(Settings& settings) {
  settings.overlayWidth = std::clamp(settings.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
  settings.maxResults = std::clamp(settings.maxResults, MIN_RESULTS, MAX_RESULT_SETTING);
  settings.clipboardHistoryLimit =
      std::clamp(settings.clipboardHistoryLimit, MIN_CLIPBOARD_HISTORY_LIMIT, MAX_CLIPBOARD_HISTORY_LIMIT);
  settings.fileIndexMaxEntries =
      std::clamp(settings.fileIndexMaxEntries, MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
}

COLORREF ColorRefFromHex(const std::wstring& hex, COLORREF fallback = RGB(0x5b, 0x6c, 0xff)) {
  if (hex.size() != 7 || hex[0] != L'#') return fallback;
  const int r = std::wcstol(hex.substr(1, 2).c_str(), nullptr, 16);
  const int g = std::wcstol(hex.substr(3, 2).c_str(), nullptr, 16);
  const int b = std::wcstol(hex.substr(5, 2).c_str(), nullptr, 16);
  return RGB(r, g, b);
}

std::wstring HexFromColorRef(COLORREF color) {
  wchar_t buf[16]{};
  swprintf_s(buf, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
  return buf;
}

D2D1_COLOR_F D2DColor(COLORREF color, float alpha = 1.0f) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha);
}

D2D1_COLOR_F D2DColor(const feathercast::theme::Color& color) {
  return D2D1::ColorF(color.r, color.g, color.b, color.a);
}

// Forces the DWM dark-mode rendering path (DWMWA_USE_IMMERSIVE_DARK_MODE = 20) so the
// window's DWM-managed chrome (border, rounded corners) uses the dark variant.
// No-op pre-Win10 2004.
inline void ApplyDarkMode(HWND hwnd) {
  constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
  BOOL enabled = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
}

// Lets DWM clip Acrylic to rounded Windows 11 corners. On older Windows versions
// this attribute is ignored and Direct2D continues to draw the visible panel shape.
inline void ApplyDwmRoundedCorners(HWND hwnd, bool enabled) {
  constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
  DWORD preference = enabled ? 2 : 1;  // DWMWCP_ROUND / DWMWCP_DONOTROUND
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

// Suppresses DWM's outer border; the app draws its own subtle Direct2D border.
inline void DisableDwmBorder(HWND hwnd) {
  constexpr DWORD DWMWA_BORDER_COLOR = 34;
  constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
  COLORREF color = kDwmColorNone;
  DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
}

// Windows 11 22H2+ (build 22621+) DWM system backdrop materials
// (DWMWA_SYSTEMBACKDROP_TYPE = 38). This is the current, documented way to get Mica/Acrylic
// frosted glass. The window's composition buffer must be fully transparent (Direct2D cleared
// to alpha 0, no opaque fill) for the material to show; otherwise the window renders black.
enum class DwmBackdropType : DWORD {
  Auto = 0,
  None = 1,
  Mica = 2,     // DWMSBT_MAINWINDOW – long-lived windows
  Acrylic = 3,  // DWMSBT_TRANSIENTWINDOW – transient popovers/overlays
  Tabbed = 4,
};

inline void ApplyModernBackdrop(HWND hwnd, DwmBackdropType type) {
  constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
  DWORD value = static_cast<DWORD>(type);
  DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &value, sizeof(value));
}

// Undocumented SetWindowCompositionAttribute accent policy. FeatherCast uses the
// long-standing blur-behind state because it remains active when Windows' global
// transparency-effects preference is disabled.
enum class AccentState : DWORD {
  Disabled = 0,
  EnableBlurBehind = 3,
};

struct AccentPolicy {
  AccentState state;
  DWORD flags;
  DWORD gradientColor;  // AABBGGRR when an accent material is enabled
  DWORD animationId;
};

struct WindowCompositionAttribData {
  DWORD attrib;  // WCA_ACCENT_POLICY = 19
  PVOID data;
  SIZE_T size;
};

inline bool ApplyAccentPolicy(HWND hwnd, AccentState state, DWORD gradientColor = 0) {
  using PfnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WindowCompositionAttribData*);
  static PfnSetWindowCompositionAttribute setAttr = []() -> PfnSetWindowCompositionAttribute {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      return reinterpret_cast<PfnSetWindowCompositionAttribute>(
          GetProcAddress(user32, "SetWindowCompositionAttribute"));
    }
    return nullptr;
  }();
  if (!setAttr) return false;
  AccentPolicy policy{state, 0, gradientColor, 0};
  WindowCompositionAttribData data{19 /*WCA_ACCENT_POLICY*/, &policy, sizeof(policy)};
  return setAttr(hwnd, &data) != FALSE;
}

COLORREF ColorRefFromTheme(const feathercast::theme::Color& color) {
  const auto toByte = [](float value) {
    return static_cast<BYTE>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  return RGB(toByte(color.r), toByte(color.g), toByte(color.b));
}

D2D1_COLOR_F Mix(COLORREF a, COLORREF b, float amountA, float alpha = 1.0f) {
  const float r = (GetRValue(a) * amountA + GetRValue(b) * (1.0f - amountA)) / 255.0f;
  const float g = (GetGValue(a) * amountA + GetGValue(b) * (1.0f - amountA)) / 255.0f;
  const float bl = (GetBValue(a) * amountA + GetBValue(b) * (1.0f - amountA)) / 255.0f;
  return D2D1::ColorF(r, g, bl, alpha);
}

uint64_t Fnv1a(const std::wstring& value) {
  uint64_t hash = 1469598103934665603ull;
  for (const wchar_t ch : value) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::filesystem::path IconCacheDir() {
  auto dir = LocalDataPath() / L"icon-cache-native";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path IconCachePath(const std::wstring& key) {
  wchar_t buf[32]{};
  swprintf_s(buf, L"%016llx.png", static_cast<unsigned long long>(Fnv1a(Lower(key))));
  return IconCacheDir() / buf;
}

bool LoadShortcut(const std::wstring& lnkPath, ShortcutInfo& info) {
  ComPtr<IShellLinkW> link;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
  ComPtr<IPersistFile> persist;
  if (FAILED(link.As(&persist))) return false;
  if (FAILED(persist->Load(lnkPath.c_str(), STGM_READ))) return false;

  wchar_t target[MAX_PATH]{};
  WIN32_FIND_DATAW findData{};
  if (SUCCEEDED(link->GetPath(target, MAX_PATH, &findData, SLGP_RAWPATH))) info.target = target;

  wchar_t args[4096]{};
  if (SUCCEEDED(link->GetArguments(args, 4096))) info.args = args;

  wchar_t cwd[MAX_PATH]{};
  if (SUCCEEDED(link->GetWorkingDirectory(cwd, MAX_PATH))) info.cwd = cwd;

  wchar_t icon[MAX_PATH]{};
  int iconIndex = 0;
  if (SUCCEEDED(link->GetIconLocation(icon, MAX_PATH, &iconIndex))) {
    info.iconPath = icon;
    info.iconIndex = iconIndex;
  }
  return true;
}

bool AltGrPressedForTextInput() {
  return (GetKeyState(VK_RMENU) & 0x8000) != 0 &&
         ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
          (GetKeyState(VK_LCONTROL) & 0x8000) != 0 ||
          (GetKeyState(VK_RCONTROL) & 0x8000) != 0);
}

void ReleaseWinModifierState() {
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = VK_LWIN;
  inputs[0].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = VK_RWIN;
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
  SendInput(static_cast<UINT>(sizeof(inputs) / sizeof(inputs[0])), inputs, sizeof(INPUT));
}

bool SendVirtualKeyTap(UINT vk) {
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = static_cast<WORD>(vk);
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = static_cast<WORD>(vk);
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(static_cast<UINT>(sizeof(inputs) / sizeof(inputs[0])), inputs,
                   sizeof(INPUT)) == std::size(inputs);
}

bool ToggleDesktop() {
  ReleaseWinModifierState();
  INPUT inputs[4]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = VK_LWIN;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = 'D';
  inputs[2] = inputs[1];
  inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
  inputs[3] = inputs[0];
  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT)) ==
         std::size(inputs);
}

std::wstring FindWindowsAppAlias(const std::wstring& fileName) {
  wchar_t local[MAX_PATH]{};
  GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (!local[0]) return L"";
  std::filesystem::path candidate = std::filesystem::path(local) / L"Microsoft" / L"WindowsApps" / fileName;
  std::error_code ec;
  return std::filesystem::exists(candidate, ec) ? candidate.wstring() : L"";
}

std::wstring ProcessPath(DWORD pid) {
  std::wstring out;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return out;
  wchar_t buf[32768]{};
  DWORD size = static_cast<DWORD>(std::size(buf));
  if (QueryFullProcessImageNameW(process, 0, buf, &size)) out.assign(buf, size);
  CloseHandle(process);
  return out;
}

std::wstring ProcessNameFromPath(const std::wstring& path) {
  if (path.empty()) return L"";
  return std::filesystem::path(path).stem().wstring();
}

std::wstring WindowTitle(HWND hwnd) {
  const int len = GetWindowTextLengthW(hwnd);
  if (len <= 0) return L"";
  std::wstring out(len + 1, L'\0');
  const int copied = GetWindowTextW(hwnd, out.data(), len + 1);
  out.resize(std::max(0, copied));
  return out;
}

bool IsRealTopLevelWindow(HWND hwnd) {
  if (!IsWindowVisible(hwnd)) return false;
  if (WindowTitle(hwnd).empty()) return false;
  const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW)) return false;
  const HWND owner = GetWindow(hwnd, GW_OWNER);
  if (owner && !(exStyle & WS_EX_APPWINDOW)) return false;
  return true;
}

struct EnumWindowContext {
  HWND own = nullptr;
  std::vector<WindowEntry> windows;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
  auto* ctx = reinterpret_cast<EnumWindowContext*>(lParam);
  if (hwnd == ctx->own || !IsRealTopLevelWindow(hwnd)) return TRUE;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return TRUE;

  WindowEntry entry;
  entry.pid = pid;
  entry.hwnd = hwnd;
  entry.name = WindowTitle(hwnd);
  entry.exe = ProcessPath(pid);
  entry.processName = ProcessNameFromPath(entry.exe);
  entry.iconKey = entry.exe;

  const std::wstring proc = Lower(entry.processName);
  if (proc == L"feathercast" || entry.name.find(L"FeatherCast") != std::wstring::npos) return TRUE;
  ctx->windows.push_back(std::move(entry));
  return TRUE;
}

std::vector<WindowEntry> ListWindows(HWND own) {
  EnumWindowContext ctx;
  ctx.own = own;
  EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
  return ctx.windows;
}

void FocusWindow(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return;
  if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
}

feathercast::window_layout::Rect LayoutRect(const RECT& rect) {
  return {rect.left, rect.top, rect.right, rect.bottom};
}

struct MonitorRecord {
  HMONITOR handle = nullptr;
  RECT bounds{};
  RECT work{};
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* records = reinterpret_cast<std::vector<MonitorRecord>*>(data);
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfoW(monitor, &info)) {
    records->push_back({monitor, info.rcMonitor, info.rcWork});
  }
  return TRUE;
}

bool ApplyWindowLayout(HWND target, feathercast::window_layout::Layout layout) {
  if (!target || !IsWindow(target)) return false;

  WINDOWPLACEMENT placement{sizeof(placement)};
  if (!GetWindowPlacement(target, &placement)) return false;
  if (IsIconic(target) || IsZoomed(target)) {
    placement.showCmd = SW_SHOWNORMAL;
    if (!SetWindowPlacement(target, &placement)) return false;
  }

  // rcNormalPosition uses workspace coordinates for ordinary top-level
  // windows. GetWindowRect after restoring gives the screen coordinates that
  // MonitorFromRect and SetWindowPos require.
  RECT normal{};
  if (!GetWindowRect(target, &normal) || normal.right <= normal.left ||
      normal.bottom <= normal.top) {
    return false;
  }

  const HMONITOR sourceMonitor =
      MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST);
  MONITORINFO sourceInfo{sizeof(sourceInfo)};
  if (!GetMonitorInfoW(sourceMonitor, &sourceInfo)) return false;

  RECT targetWork = sourceInfo.rcWork;
  if (layout == feathercast::window_layout::Layout::NextDisplay ||
      layout == feathercast::window_layout::Layout::PreviousDisplay) {
    std::vector<MonitorRecord> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) return false;
    std::sort(monitors.begin(), monitors.end(), [](const auto& left,
                                                   const auto& right) {
      return feathercast::window_layout::PositionLess(
          LayoutRect(left.bounds), LayoutRect(right.bounds));
    });
    const auto current = std::find_if(
        monitors.begin(), monitors.end(), [&](const MonitorRecord& record) {
          return record.handle == sourceMonitor;
        });
    const std::size_t currentIndex =
        current == monitors.end()
            ? 0
            : static_cast<std::size_t>(
                  std::distance(monitors.begin(), current));
    const std::size_t index =
        layout == feathercast::window_layout::Layout::PreviousDisplay
            ? feathercast::window_layout::PreviousIndex(currentIndex,
                                                        monitors.size())
            : feathercast::window_layout::NextIndex(currentIndex,
                                                    monitors.size());
    targetWork = monitors[index].work;
  }

  const auto destination = feathercast::window_layout::Compute(
      layout, LayoutRect(normal), LayoutRect(sourceInfo.rcWork),
      LayoutRect(targetWork));
  return SetWindowPos(target, nullptr, destination.left, destination.top,
                      destination.Width(), destination.Height(),
                      SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
}

enum class UpdateTaskKind {
  Check,
  DownloadInstall,
};

enum class UpdateTaskStatus {
  NoUpdate,
  Available,
  ReadyToInstall,
  Error,
};

struct UpdateTaskResult {
  bool manual = false;
  UpdateTaskKind kind = UpdateTaskKind::Check;
  UpdateTaskStatus status = UpdateTaskStatus::Error;
  feathercast::updater::ReleaseInfo release;
  std::filesystem::path installerPath;
  std::wstring message;
};

using NetworkEvent = std::variant<CurrencyRates, UpdateTaskResult>;

struct ExtensionActivationResult {
  std::optional<feathercast::extensions::ActivationResponse> response;
};

struct LaunchCompletion {
  std::wstring id;
  std::wstring name;
  bool succeeded = false;
};

class FeatherCastApp;
FeatherCastApp* g_app = nullptr;

class FeatherCastApp : public feathercast::accessibility::Model {
 public:
  explicit FeatherCastApp(HINSTANCE instance, std::wstring cmdLine)
      : persistenceEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        persistence_(
            SettingsPath(), DatabasePath(),
            [this](feathercast::persistence::Event event) {
              persistenceEvents_.Push(std::move(event));
            }),
        searchEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        searchCoordinator_([this](ResultsCollection result) {
          searchEvents_.Push(std::move(result));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        snapshotCoordinator_([this](SnapshotBuildResult result) {
          searchEvents_.Push(std::move(result));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        discoveryEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        discoveryService_([this](DiscoveryResult result) {
          discoveryEvents_.Push(std::move(result));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        fileIndexEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        fileIndexService_([this](feathercast::files::IndexStatus status) {
          fileIndexEvents_.Push(std::move(status));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        fileSearchEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        fileSearchService_(DatabasePath(), [this](ResultsCollection result) {
          fileSearchEvents_.Push(std::move(result));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        previewEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        previewService_([this](feathercast::preview::Result result) {
          previewEvents_.Push(std::move(result));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        iconEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        iconResolver_([this](feathercast::runtime::DecodedIcon icon) {
          iconEvents_.Push(std::move(icon));
        }, [this](std::exception_ptr) {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
          }
        }),
        networkEvents_([this] {
          if (hwnd_ && !stopThreads_) {
            PostMessageW(hwnd_, WM_APP_EVENTS, 0, 0);
          }
        }),
        currencyService_([this](CurrencyRates rates) {
          networkEvents_.Push(std::move(rates));
        }),
        updateService_([this](UpdateTaskResult result) {
          networkEvents_.Push(std::move(result));
        }),
        instance_(instance),
        cmdLine_(std::move(cmdLine)) {
    QueryPerformanceFrequency(&qpcFrequency_);
    overlayOpacity_.Snap(1.0);
    settingsOpacity_.Snap(1.0);
    volumeOpacity_.Snap(1.0);
    confirmationProgress_.Snap(0.0);
    overlayVisualScroll_.Snap(0.0);
    settingsVisualScroll_.Snap(0.0);
    settingsPageProgress_.Snap(1.0);
    settingsCategoryTop_.Snap(0.0);
    volumeVisualPercent_.Snap(0.0);
    auto loadedSettings = persistence_.LoadSettingsForStartup();
    settings_ = std::move(loadedSettings.value);
    ClampSettings(settings_);
    settingsPersistenceBlocked_ = !loadedSettings.persistenceAllowed;
    startupSettingsNotice_ = std::move(loadedSettings.message);
    settingsRecoveredFromInvalidFile_ =
        loadedSettings.status == feathercast::settings::ParseStatus::Invalid;
    g_diagnosticsEnabled = settings_.diagnosticsEnabled;
    hadLegacyOperationalData_ = MigrateLegacyOperationalData();
    shortcut_ = ParseShortcut(settings_.shortcut);
    theme_ = feathercast::theme::LoadTheme(ThemePath());
    RefreshSystemPreferences();
    const auto loadedSnippets = feathercast::snippets_io::Load(SnippetsPath());
    snippets_ = loadedSnippets.snippets;
    snippetsFingerprint_ = loadedSnippets.fingerprint;
    snippetsWritable_ = loadedSnippets.Writable();
    snippetsLoadMessage_ = loadedSnippets.message;
    systemFolders_ = SystemFolderEntries();
    if (!SetStartOnStartup(settings_.startOnStartup) && settings_.startOnStartup) {
      settings_.startOnStartup = false;
      if (!settingsPersistenceBlocked_) {
        persistence_.SaveSettingsForStartup(settings_);
      }
    }
  }

  ~FeatherCastApp() {
    ShutdownBackgroundWorkers();
    if (clipboardListenerRegistered_ && hwnd_) RemoveClipboardFormatListener(hwnd_);
    UnregisterShortcutHotKey();
    if (hook_) UnhookWindowsHookEx(hook_);
    RemoveTray();
  }

  void ShutdownBackgroundWorkers() {
    if (shutdownStarted_.exchange(true)) return;
    stopThreads_ = true;
    launchExecutor_.Stop();
    extensions_.Shutdown();
    fileIndexService_.Stop();
    fileIndexEvents_.Close();
    discoveryService_.Stop();
    discoveryEvents_.Close();
    previewService_.Stop();
    previewEvents_.Close();
    fileSearchService_.Stop();
    fileSearchEvents_.Close();
    searchCoordinator_.Stop();
    snapshotCoordinator_.Stop();
    searchEvents_.Close();
    persistence_.Stop(true);
    persistenceEvents_.Close();
    currencyService_.Stop();
    updateService_.Stop();
    networkEvents_.Close();
    StopIconThreads();
    iconEvents_.Close();
  }

  int Run() {
    g_app = this;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coHr)) {
      MessageBoxW(nullptr, L"FeatherCast could not initialize the Windows COM apartment.",
                  L"FeatherCast Startup", MB_OK | MB_ICONERROR);
      g_app = nullptr;
      return 1;
    }
    if (!InitializeFactories() || !RegisterWindowClass() || !CreateMainWindow() ||
        !CreateSettingsWindow() || !CreateVolumeWindow()) {
      MessageBoxW(nullptr, L"FeatherCast could not initialize its native windows or graphics factories.",
                  L"FeatherCast Startup", MB_OK | MB_ICONERROR);
      ReleaseComResources();
      CoUninitialize();
      g_app = nullptr;
      return 1;
    }
    persistence_.Start();
    fileSearchService_.Start();
    previewService_.Start();
    PromptForPrivacyConsentIfNeeded();
    LoadPersistentState();
    UpdateClipboardListenerRegistration();
    if (!CreateTray()) {
      MessageBoxW(hwnd_, L"FeatherCast could not create its tray icon.",
                  L"FeatherCast Startup", MB_OK | MB_ICONWARNING);
    }
    if (!startupSettingsNotice_.empty()) {
      SetSettingsStatus(settingsPersistenceBlocked_ ? StatusSeverity::Error
                                                    : StatusSeverity::Info,
                        startupSettingsNotice_);
      ShowTrayNotification(
          settingsRecoveredFromInvalidFile_ ? L"FeatherCast Settings Recovery"
                                            : L"FeatherCast Settings",
          settingsPersistenceBlocked_
              ? L"Settings could not be preserved; automatic saves are disabled."
              : L"Invalid settings were preserved and defaults are active.");
    }
    extensions_.Initialize(UserDataPath(), ExeDirectory(), hwnd_, WM_REBUILD_RESULTS);
    const bool hookReady = InstallHook();
    const bool hotKeyReady = RegisterShortcutHotKey();
    if (!hotKeyReady && !hookReady) {
      MessageBoxW(hwnd_, L"FeatherCast could not register the global shortcut or keyboard hook.",
                  L"FeatherCast Startup", MB_OK | MB_ICONWARNING);
    }
    auto launchFailure = [this](std::exception_ptr) {
      if (!stopThreads_) PostMessageW(hwnd_, WM_BACKGROUND_TASK_ERROR, 0, 0);
    };
    launchExecutor_.Start(2, launchFailure);
    searchCoordinator_.Start([](const QueryRequest& request) {
      return ComputeResults(request);
    });
    snapshotCoordinator_.Start([this](const Settings& settings) {
      return BuildSnapshot(settings);
    });
    discoveryService_.Start(
        [this](const DiscoveryRequest& request, std::stop_token token) {
          return PerformDiscovery(request, token);
        });
    StartAppDiscovery();
    StartCurrencyFetch();
    StartAutomaticUpdateCheck();

    if (cmdLine_.find(L"--show") != std::wstring::npos) {
      ShowOverlay(View::Search);
    } else {
      UpdateBackgroundState();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    ShutdownBackgroundWorkers();
    g_app = nullptr;
    ReleaseComResources();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
  }

  bool IsFeatherCastWindow(HWND window) const {
    return window &&
           (window == hwnd_ || window == settingsHwnd_ || window == volumeHwnd_);
  }

  std::optional<ForegroundSnapshot> CaptureForegroundSnapshot(
      HWND window, DWORD inputTime = 0) const {
    if (!window || !IsWindow(window)) return std::nullopt;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root) root = window;
    if (!IsWindow(root) || IsFeatherCastWindow(root)) return std::nullopt;
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(root, &processId);
    if (!threadId || !processId) return std::nullopt;
    return ForegroundSnapshot{root, processId, threadId, inputTime,
                              GetTickCount64()};
  }

  bool ValidateRestoreCandidate(const RestoreCandidate& candidate) const {
    if (!candidate.hwnd || !IsWindow(candidate.hwnd) ||
        IsFeatherCastWindow(candidate.hwnd) ||
        GetAncestor(candidate.hwnd, GA_ROOT) != candidate.hwnd) {
      return false;
    }
    DWORD processId = 0;
    const DWORD threadId =
        GetWindowThreadProcessId(candidate.hwnd, &processId);
    const feathercast::interaction::OverlayRestoreIdentity expected{
        reinterpret_cast<std::uintptr_t>(candidate.hwnd), candidate.processId,
        candidate.threadId, candidate.generation};
    return feathercast::interaction::OverlayRestoreIdentityMatches(
        expected, reinterpret_cast<std::uintptr_t>(candidate.hwnd), processId,
        threadId, candidate.generation);
  }

  void QueueShortcutToggle(
      std::optional<ForegroundSnapshot> foreground,
      bool deferUntilWinRelease = false) {
    ShortcutToggleRequest request;
    request.id = ++nextShortcutToggleRequestId_;
    request.foreground = std::move(foreground);
    request.deferUntilWinRelease = deferUntilWinRelease;
    shortcutToggleRequests_.push_back(std::move(request));
    if (!PostMessageW(hwnd_, WM_SHORTCUT_TOGGLE,
                      static_cast<WPARAM>(nextShortcutToggleRequestId_), 0)) {
      shortcutToggleRequests_.pop_back();
    }
  }

  std::optional<ForegroundSnapshot> TakeShortcutToggleRequest(
      std::uint64_t id) {
    const auto found = std::find_if(
        shortcutToggleRequests_.begin(), shortcutToggleRequests_.end(),
        [id](const ShortcutToggleRequest& request) { return request.id == id; });
    if (found == shortcutToggleRequests_.end()) return std::nullopt;
    auto foreground = std::move(found->foreground);
    shortcutToggleRequests_.erase(found);
    return foreground;
  }

  bool ShortcutToggleNeedsWinRelease(std::uint64_t id) const {
    const auto found = std::find_if(
        shortcutToggleRequests_.begin(), shortcutToggleRequests_.end(),
        [id](const ShortcutToggleRequest& request) { return request.id == id; });
    return found != shortcutToggleRequests_.end() &&
           found->deferUntilWinRelease;
  }

  void DispatchShortcutToggle(std::uint64_t id) {
    if (!recording_) {
      TriggerShortcutToggle(TakeShortcutToggleRequest(id));
    } else {
      TakeShortcutToggleRequest(id);
    }
  }

  LRESULT LowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(hook_, nCode, wParam, lParam);
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (!kb || (kb->flags & LLKHF_INJECTED)) return CallNextHookEx(hook_, nCode, wParam, lParam);

    const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    const UINT vk = kb->vkCode;

    if (recording_) {
      return HandleRecordingKey(vk, down, up);
    }

    if (ShouldHandleInLowLevelHook(shortcut_, hotKeyRegistered_)) {
      PressedModifiers modifiers = hookModifiers_;
      if (down && IsModifier(vk)) {
        SetHookModifier(vk, true);
        modifiers = hookModifiers_;
      }
      const auto result = shortcutRuntime_.Handle(shortcut_, vk, down, up, modifiers);
      if (up && IsModifier(vk)) SetHookModifier(vk, false);
      if (result.suppressWinStart) {
        SendVirtualKeyTap(0xE8);
      }
      if (result.toggle && hwnd_) {
        QueueShortcutToggle(
            CaptureForegroundSnapshot(GetForegroundWindow(), kb->time),
            result.deferToggleUntilWinRelease);
      }
      if (result.consume) return 1;
    }
    return CallNextHookEx(hook_, nCode, wParam, lParam);
  }

  std::wstring AccessibleWindowName(HWND hwnd) const override {
    if (hwnd == settingsHwnd_) return L"FeatherCast Settings";
    if (hwnd == volumeHwnd_) return L"FeatherCast Volume Control";
    return L"FeatherCast Launcher";
  }

  std::vector<feathercast::accessibility::Item> AccessibleItems(HWND hwnd) const override {
    using feathercast::accessibility::Item;
    std::vector<Item> items;
    const float scale = GetWindowScale(hwnd);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    RECT client{};
    GetClientRect(hwnd, &client);
    auto screenRect = [&](RectF rect) {
      return RECT{
        origin.x + static_cast<LONG>(rect.left * scale),
        origin.y + static_cast<LONG>(rect.top * scale),
        origin.x + static_cast<LONG>(rect.right * scale),
        origin.y + static_cast<LONG>(rect.bottom * scale),
      };
    };

    if (hwnd == volumeHwnd_) {
      const float width = static_cast<float>(client.right) / scale;
      Item slider;
      slider.name = L"Volume";
      slider.value = std::to_wstring(volumePercent_) + L"%";
      slider.description = volumeStatus_.empty()
                               ? L"Use arrow keys to adjust the default output volume"
                               : volumeStatus_;
      slider.role = ROLE_SYSTEM_SLIDER;
      slider.state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED;
      slider.screenRect = screenRect(VolumeTrackHitRect(width));
      items.push_back(std::move(slider));
      return items;
    }

    if (hwnd == hwnd_) {
      if (confirmation_) {
        const float width = static_cast<float>(client.right) / scale;
        const float height = static_cast<float>(client.bottom) / scale;
        const auto [cancelRect, actionRect] = ConfirmationButtonRects(width, height);

        Item cancel;
        cancel.name = L"Cancel";
        cancel.defaultAction = L"Cancel";
        cancel.role = ROLE_SYSTEM_PUSHBUTTON;
        cancel.state = STATE_SYSTEM_FOCUSABLE;
        if (confirmationFocus_ == 0) cancel.state |= STATE_SYSTEM_FOCUSED;
        cancel.screenRect = screenRect(cancelRect);
        items.push_back(std::move(cancel));

        Item confirm;
        confirm.name = confirmation_->actionLabel;
        confirm.description = confirmation_->message;
        confirm.defaultAction = confirmation_->actionLabel;
        confirm.role = ROLE_SYSTEM_PUSHBUTTON;
        confirm.state = STATE_SYSTEM_FOCUSABLE;
        if (confirmationFocus_ == 1) confirm.state |= STATE_SYSTEM_FOCUSED;
        confirm.screenRect = screenRect(actionRect);
        items.push_back(std::move(confirm));
        return items;
      }

      Item search;
      search.name = L"Search";
      search.value = query_;
      search.description = SearchPending()
                               ? L"Searching; " + std::to_wstring(flatItems_.size()) +
                                     L" previous results unavailable"
                               : std::to_wstring(flatItems_.size()) + L" results";
      search.role = ROLE_SYSTEM_TEXT;
      search.state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED;
      search.screenRect = screenRect({52, 12, static_cast<float>(client.right) / scale - 94, 50});
      items.push_back(std::move(search));

      float y = kResultsTop -
                static_cast<float>(overlayVisualScroll_.Value());
      int index = 0;
      for (const auto& section : sections_) {
        y += kSectionHeaderHeight;
        for (const auto& display : section.items) {
          Item result;
          result.name = display.Name();
          result.description = SourceLabel(display);
          result.defaultAction = ActionHint(display);
          result.role = ROLE_SYSTEM_LISTITEM;
          result.state = STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_FOCUSABLE;
          if (SearchPending()) {
            result.state |= STATE_SYSTEM_UNAVAILABLE;
            result.defaultAction.clear();
          } else if (index == selected_) {
            result.state |= STATE_SYSTEM_SELECTED | STATE_SYSTEM_FOCUSED;
          }
          result.screenRect = screenRect({8, y, static_cast<float>(client.right) / scale - 8,
                                          y + kResultRowHeight});
          if (result.screenRect.bottom < origin.y ||
              result.screenRect.top > origin.y + client.bottom) {
            result.state |= STATE_SYSTEM_OFFSCREEN;
          }
          items.push_back(std::move(result));
          y += kResultRowStride;
          ++index;
        }
      }
      return items;
    }

    const auto order = SettingsFocusOrder();
    auto nameFor = [](HitType type) {
      if (const auto* category =
              feathercast::settings_catalog::FindCategory(type)) {
        return category->accessibleName.data();
      }
      if (const auto* setting = feathercast::settings_catalog::Find(type)) {
        return setting->accessibleName.data();
      }
      switch (type) {
        case HitType::CloseSettings: return L"Close settings";
        default: return L"Setting";
      }
    };
    auto checked = [&](HitType type) {
      return feathercast::settings_catalog::Checked(type, settings_);
    };
    for (size_t orderIndex = 0; orderIndex < order.size(); ++orderIndex) {
      const HitType type = order[orderIndex];
      const auto hit = std::find_if(hits_.begin(), hits_.end(),
                                    [&](const HitTarget& target) { return target.type == type; });
      if (hit == hits_.end()) continue;
      Item setting;
      setting.name = nameFor(type);
      const auto category = SettingsCategoryForHit(type);
      setting.defaultAction = category
                                  ? L"Select"
                                  : (type == HitType::AnimationLevel
                                         ? L"Change animation level"
                                         : L"Activate");
      if (category) {
        setting.role = ROLE_SYSTEM_PAGETAB;
      } else if (type == HitType::AnimationLevel) {
        setting.role = ROLE_SYSTEM_SLIDER;
        setting.value = std::wstring(
            feathercast::settings::AnimationLevelLabel(settings_.animationLevel));
        setting.description =
            L"Off, Reduced, or Full. Use arrow keys to adjust.";
      } else {
        setting.role =
            type == HitType::StartupToggle ||
                    type == HitType::UpdateChecksToggle ||
                    type == HitType::CompactToggle ||
                    type == HitType::ShowWindowsToggle ||
                    type == HitType::ShowStoreAppsToggle ||
                    type == HitType::ClipboardHistoryToggle ||
                    type == HitType::FileIndexToggle ||
                    type == HitType::FileContentIndexToggle ||
                    type == HitType::DiagnosticsToggle ||
                    type == HitType::AccentToggle
                ? ROLE_SYSTEM_CHECKBUTTON
                : ROLE_SYSTEM_PUSHBUTTON;
      }
      setting.state = STATE_SYSTEM_FOCUSABLE;
      if (category) setting.state |= STATE_SYSTEM_SELECTABLE;
      if (checked(type)) setting.state |= STATE_SYSTEM_CHECKED;
      if (category && *category == settingsCategory_) setting.state |= STATE_SYSTEM_SELECTED;
      if (static_cast<int>(orderIndex) == settingsFocusIndex_) {
        setting.state |= STATE_SYSTEM_FOCUSED;
      }
      setting.screenRect = screenRect(hit->rect);
      items.push_back(std::move(setting));
    }
    constexpr std::array disabledCandidates = {
      HitType::ClipboardLimitDown,
      HitType::ClipboardLimitUp,
      HitType::FileIndexLimitDown,
      HitType::FileIndexLimitUp,
      HitType::FileContentIndexToggle,
      HitType::AddFileRoot,
      HitType::RemoveFileRoot,
      HitType::ClearFileRoots,
      HitType::RebuildFileIndex,
      HitType::ClearClipboardData,
      HitType::ClearFileIndexData,
      HitType::ReloadExtensions,
    };
    for (const HitType type : disabledCandidates) {
      if (SettingsControlEnabled(type)) continue;
      const auto hit = std::find_if(hits_.begin(), hits_.end(),
                                    [&](const HitTarget& target) {
                                      return target.type == type;
                                    });
      if (hit == hits_.end()) continue;
      Item setting;
      setting.name = nameFor(type);
      setting.role = ROLE_SYSTEM_PUSHBUTTON;
      setting.state = STATE_SYSTEM_UNAVAILABLE;
      setting.screenRect = screenRect(hit->rect);
      items.push_back(std::move(setting));
    }
    if (settingsCategory_ == SettingsCategory::Extensions) {
      const float width = static_cast<float>(client.right) / scale;
      float y = kSettTop + SettingsStatusOffset() + kSettSection + kSettRow -
                static_cast<float>(settingsVisualScroll_.Value());
      for (const auto& plugin : extensions_.Health()) {
        Item status;
        status.name = plugin.name;
        status.value = plugin.version;
        status.description =
            !plugin.available
                ? L"Unavailable"
                : (plugin.failureStrikes > 0 ? L"Degraded" : L"Available");
        if (!plugin.lastError.empty()) {
          status.description += L": " + plugin.lastError;
        }
        status.role = ROLE_SYSTEM_STATICTEXT;
        status.state = STATE_SYSTEM_READONLY;
        status.screenRect = screenRect(
            {kSettSidebarWidth + kSettContentInset, y,
             width - kSettContentInset, y + kSettRow});
        items.push_back(std::move(status));
        y += kSettRow;
      }
    }
    if (settingsStatus_) {
      Item status;
      status.name = settingsStatus_->text;
      status.role = settingsStatus_->severity == StatusSeverity::Error
                        ? ROLE_SYSTEM_ALERT
                        : ROLE_SYSTEM_STATICTEXT;
      status.state = STATE_SYSTEM_READONLY;
      const float width = static_cast<float>(client.right) / scale;
      status.screenRect =
          screenRect({kSettSidebarWidth + kSettContentInset, kSettTop,
                      width - kSettContentInset, kSettTop + 40.0f});
      items.push_back(std::move(status));
    }
    return items;
  }

  int AccessibleFocusedChild(HWND hwnd) const override {
    if (hwnd == volumeHwnd_) return 1;
    if (hwnd == hwnd_) {
      if (confirmation_) return confirmationFocus_ + 1;
      return flatItems_.empty() || SearchPending() ? 1 : selected_ + 2;
    }
    return settingsFocusIndex_ + 1;
  }

  void AccessibleFocusChild(HWND hwnd, int child) override {
    if (hwnd == volumeHwnd_) {
      if (child == 1) SetFocus(volumeHwnd_);
      return;
    }
    if (hwnd == hwnd_) {
      if (confirmation_) {
        confirmationFocus_ = std::clamp(child - 1, 0, 1);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }
      if (child <= 1) {
        SetFocus(hwnd_);
      } else if (!SearchPending()) {
        SelectResult(child - 2, false, true);
      }
      return;
    }
    const auto order = SettingsFocusOrder();
    feathercast::ui::SettingsController::SetFocus(
        settingsState_, child - 1, static_cast<int>(order.size()));
    if (const auto category =
            SettingsCategoryForHit(order[static_cast<size_t>(settingsFocusIndex_)])) {
      SelectSettingsCategory(*category);
    }
    EnsureSettingsFocusVisible();
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void AccessibleInvokeChild(HWND hwnd, int child) override {
    if (hwnd == volumeHwnd_) {
      if (child == 1) SetFocus(volumeHwnd_);
      return;
    }
    if (hwnd == hwnd_) {
      if (confirmation_) {
        confirmationFocus_ = std::clamp(child - 1, 0, 1);
        ActivateConfirmationChoice();
        return;
      }
      const int index = child - 2;
      ActivateResultAt(index, false);
      return;
    }
    const auto order = SettingsFocusOrder();
    const int index = child - 1;
    if (index < 0 || index >= static_cast<int>(order.size())) return;
    const HitType type = order[static_cast<size_t>(index)];
    if (type == HitType::CloseSettings) HideSettings();
    else HandleSettingsHit(type);
  }

 private:
  static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FeatherCastApp* app = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      app = reinterpret_cast<FeatherCastApp*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
      app = reinterpret_cast<FeatherCastApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  static LRESULT CALLBACK StaticKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return g_app ? g_app->LowLevelKeyboard(nCode, wParam, lParam) : CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (hwnd == settingsHwnd_) return SettingsWndProc(hwnd, msg, wParam, lParam);
    if (hwnd == volumeHwnd_) return VolumeWndProc(hwnd, msg, wParam, lParam);
    if (taskbarCreatedMessage_ && msg == taskbarCreatedMessage_) {
      RemoveTray();
      CreateTray();
      return 0;
    }
    switch (msg) {
      case WM_TIMER:
        if (wParam == 1) {
          if (visible_) {
            if (!suppressHide_ && !overlayClosing_ &&
                !overlayFocusSession_.GuardActive(GetTickCount64())) {
              HWND foreground = GetForegroundWindow();
              if (foreground && foreground != hwnd_) {
                HideOverlay(OverlayCloseReason::PassiveFocusLoss);
                return 0;
              }
            }
            // Only repaint when the caret blink phase actually flips; an otherwise
            // idle overlay no longer re-renders five times a second.
            const bool phase = CaretPhase();
            if (phase != lastCaretPhase_) {
              lastCaretPhase_ = phase;
              InvalidateRect(hwnd_, nullptr, FALSE);
            }
          }
        } else if (wParam == TIMER_MEM_TRIM) {
          KillTimer(hwnd_, TIMER_MEM_TRIM);
          SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        } else if (wParam == TIMER_OVERLAY_ACTIVATE) {
          RetryOverlayActivation();
        } else if (wParam == TIMER_PREVIEW_LOAD) {
          KillTimer(hwnd_, TIMER_PREVIEW_LOAD);
          LoadSelectedPreview();
        } else if (wParam == TIMER_FILE_SNAPSHOT) {
          KillTimer(hwnd_, TIMER_FILE_SNAPSHOT);
          MarkSearchDataChanged();
          if (visible_) RequestSearch();
        } else if (wParam == TIMER_SHORTCUT_TOGGLE) {
          if (!pendingShortcutToggleRequestId_) {
            KillTimer(hwnd_, TIMER_SHORTCUT_TOGGLE);
          } else if (!ModifierPressed(VK_LWIN)) {
            const std::uint64_t requestId = pendingShortcutToggleRequestId_;
            pendingShortcutToggleRequestId_ = 0;
            KillTimer(hwnd_, TIMER_SHORTCUT_TOGGLE);
            DispatchShortcutToggle(requestId);
          }
        }
        return 0;
      case WM_CREATE:
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_GETOBJECT:
        if (static_cast<LONG>(lParam) == OBJID_CLIENT) {
          return feathercast::accessibility::HandleGetObject(this, hwnd, wParam, lParam);
        }
        break;
      case WM_DWMCOMPOSITIONCHANGED:
      case WM_THEMECHANGED:
      case WM_SETTINGCHANGE:
      case WM_DISPLAYCHANGE:
        RefreshSystemPreferences();
        ApplyGlass(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_CLIPBOARDUPDATE:
        OnClipboardUpdate();
        return 0;
      case WM_HOTKEY:
        if (static_cast<int>(wParam) == HOTKEY_OPEN_SEARCH) {
          if (!recording_) {
            TriggerShortcutToggle(CaptureForegroundSnapshot(
                GetForegroundWindow(), static_cast<DWORD>(GetMessageTime())));
          }
          return 0;
        }
        break;
      case WM_SHORTCUT_TOGGLE:
        if (const std::uint64_t requestId = static_cast<std::uint64_t>(wParam);
            ShortcutToggleNeedsWinRelease(requestId) &&
            ModifierPressed(VK_LWIN)) {
          pendingShortcutToggleRequestId_ = requestId;
          if (!SetTimer(hwnd_, TIMER_SHORTCUT_TOGGLE, 1, nullptr)) {
            pendingShortcutToggleRequestId_ = 0;
            DispatchShortcutToggle(requestId);
          }
        } else {
          DispatchShortcutToggle(requestId);
        }
        return 0;
      case WM_OVERLAY_RETRY_ACTIVATION:
        RetryOverlayActivation();
        return 0;
      case WM_DESTROY:
        UnregisterShortcutHotKey();
        if (settingsHwnd_) {
          HWND settings = settingsHwnd_;
          settingsHwnd_ = nullptr;
          DestroyWindow(settings);
        }
        if (volumeHwnd_) {
          HWND volume = volumeHwnd_;
          volumeHwnd_ = nullptr;
          DestroyWindow(volume);
        }
        PostQuitMessage(0);
        return 0;
      case WM_PAINT:
        Paint();
        return 0;
      case WM_SIZE:
        ResizeGlassSurface(overlaySurface_, hwnd_, LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_DPICHANGED: {
        auto prc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ResetTextFormats();
        DiscardGlassDevice();
        ApplyRoundedRegion(prc->right - prc->left, prc->bottom - prc->top);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
          CancelPointerPress(hwnd);
          HandleOverlayFocusLoss(reinterpret_cast<HWND>(lParam));
        }
        return 0;
      case WM_KILLFOCUS:
        CancelPointerPress(hwnd);
        HandleOverlayFocusLoss(reinterpret_cast<HWND>(wParam));
        return 0;
      case WM_CANCELMODE:
      case WM_CAPTURECHANGED:
        CancelPointerPress(hwnd);
        return 0;
      case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        break;
      case WM_KEYDOWN:
        OnKeyDown(static_cast<UINT>(wParam));
        return 0;
      case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;
      case WM_IME_STARTCOMPOSITION:
        imeComposition_.clear();
        return 0;
      case WM_IME_COMPOSITION:
        OnImeComposition(lParam);
        return 0;
      case WM_IME_ENDCOMPOSITION:
        imeComposition_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_SYSCHAR:
        if (AltGrPressedForTextInput()) OnChar(static_cast<wchar_t>(wParam));
        return 0;
      case WM_MOUSEMOVE: {
        const float scale = GetWindowScale(hwnd_);
        OnMouseMove(static_cast<float>(GET_X_LPARAM(lParam)) / scale, static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_MOUSELEAVE:
        mouseTracking_ = false;
        if (confirmationHover_ != -1) {
          confirmationHover_ = -1;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (gearHovered_) {
          gearHovered_ = false;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      case WM_LBUTTONDOWN: {
        const float scale = GetWindowScale(hwnd_);
        OnPointerDown(hwnd,
                      static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                      static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_LBUTTONUP: {
        const float scale = GetWindowScale(hwnd_);
        OnPointerUp(hwnd,
                    static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                    static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_RBUTTONUP: {
        const float scale = GetWindowScale(hwnd_);
        OnRightClick(static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                     static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_CONTEXTMENU: {
        if (static_cast<short>(LOWORD(lParam)) == -1 &&
            static_cast<short>(HIWORD(lParam)) == -1) {
          OpenSelectedResultActions();
        } else {
          POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
          ScreenToClient(hwnd_, &point);
          const float scale = GetWindowScale(hwnd_);
          OnRightClick(static_cast<float>(point.x) / scale,
                       static_cast<float>(point.y) / scale);
        }
        return 0;
      }
      case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
      case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
          fileIndexService_.Rebuild();
        }
        return TRUE;
      case WM_TRAYICON:
        OnTray(lParam);
        return 0;
      case WM_SHOW_SEARCH:
        ShowOverlay(View::Search);
        return 0;
      case WM_REBUILD_RESULTS:
        // Data-mutation sites (apps_/windows_/fileIndex_/snippets_/clipboard) and
        // Settings/data changes advance the corpus revision themselves; this also fires for
        // async extension results, which do not affect the cached corpus.
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_TRACK_RECENT: {
        std::deque<LaunchCompletion> completions;
        {
          std::lock_guard lock(completedLaunchMutex_);
          completions.swap(completedLaunches_);
        }
        for (const auto& completion : completions) {
          if (completion.succeeded) {
            TrackRecent(completion.id);
          } else {
            ShowTrayNotification(
                L"FeatherCast Launch Failed",
                completion.name.empty()
                    ? L"Windows could not open the selected item."
                    : L"Windows could not open " + completion.name + L".");
          }
        }
        return 0;
      }
      case WM_ANIMATION_FRAME:
        OnAnimationFrame();
        return 0;
      case WM_EXTENSION_ACTIVATED:
        OnExtensionActivationReady();
        return 0;
      case WM_APP_EVENTS:
        OnRuntimeEvents();
        return 0;
      case WM_RENDER_RECOVER:
        renderRecoveryQueued_ = false;
        // Recreate the shared device stack from a normal app message, never
        // recursively from the failing WM_PAINT call.
        EnsureGlassDevice();
        if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
        if (settingsHwnd_ && IsWindowVisible(settingsHwnd_)) {
          InvalidateRect(settingsHwnd_, nullptr, FALSE);
        }
        if (volumeVisible_) InvalidateRect(volumeHwnd_, nullptr, FALSE);
        return 0;
      case WM_EXTENSION_RELOAD_READY:
        extensionReloadPending_ = false;
        SetSettingsStatus(StatusSeverity::Success, L"Extensions reloaded.");
        return 0;
      case WM_BACKGROUND_TASK_ERROR:
        extensionReloadPending_ = false;
        SetSettingsStatus(
            StatusSeverity::Error,
            L"A background task failed. FeatherCast kept running; retry the operation.");
        if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) {
          ShowTrayNotification(L"FeatherCast",
                               L"A background task failed. FeatherCast kept running.");
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  void OnRuntimeEvents() {
    OnPersistenceEvents();
    for (auto& queued : searchEvents_.Drain()) {
      std::visit(
          [this](auto&& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, ResultsCollection>) {
              searchPresentation_.MarkCompleted(event.generation);
              if (event.generation == searchPresentation_.Requested()) {
                ApplyResults(std::move(event));
                InvalidateRect(hwnd_, nullptr, FALSE);
              }
            } else if constexpr (std::is_same_v<Event,
                                                SnapshotBuildResult>) {
              OnSnapshotReady(std::move(event));
            }
          },
          std::move(queued));
    }
    for (auto& result : discoveryEvents_.Drain()) {
      OnDiscoveryCompleted(std::move(result));
    }
    for (auto& status : fileIndexEvents_.Drain()) {
      OnFileIndexReady(std::move(status));
    }
    for (auto& result : fileSearchEvents_.Drain()) {
      searchPresentation_.MarkCompleted(result.generation);
      if (result.generation == searchPresentation_.Requested()) {
        ApplyResults(std::move(result));
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
    }
    for (auto& result : previewEvents_.Drain()) {
      if (result.generation != previewGeneration_ || !previewOpen_) continue;
      previewResult_ = std::move(result);
      previewBitmap_.Reset();
      previewScroll_ = 0.0f;
      InvalidateRect(hwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_REORDER, hwnd_, OBJID_CLIENT, CHILDID_SELF);
    }
    auto decodedIcons = iconEvents_.Drain();
    // Icons are only useful while the search overlay is visible.  Discovery
    // and icon workers can finish after the overlay was dismissed; retaining
    // those decoded pixel buffers would otherwise grow the hidden process.
    if (visible_) {
      for (auto& icon : decodedIcons) {
        StorePendingDecodedIcon(std::move(icon));
      }
    }
    if (visible_ && !decodedIcons.empty()) {
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
    for (auto& queued : networkEvents_.Drain()) {
      std::visit(
          [this](auto&& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, CurrencyRates>) {
              {
                std::lock_guard lock(dataMutex_);
                currencyRates_ = std::move(event);
              }
              RequestSearch();
            } else if constexpr (std::is_same_v<Event,
                                                UpdateTaskResult>) {
              OnUpdateReady(std::move(event));
            }
          },
          std::move(queued));
    }
  }

  void OnPersistenceEvents() {
    for (auto& queued : persistenceEvents_.Drain()) {
      std::visit(
          [this](auto&& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<
                              Event,
                              feathercast::persistence::
                                  SettingsSaveCompleted>) {
              if (event.succeeded) {
                if (!settingsPersistenceBlocked_ && settingsStatus_ &&
                    settingsStatus_->severity == StatusSeverity::Progress &&
                    settingsStatus_->text == L"Saving settings...") {
                  SetSettingsStatus(StatusSeverity::Success,
                                    L"All changes saved.");
                }
              } else {
                ReportPersistenceFailure(event.error);
              }
            } else if constexpr (std::is_same_v<
                                     Event,
                                     feathercast::persistence::
                                         StorageClearCompleted>) {
              StorageOperationResult result;
              result.kind = event.kind;
              result.succeeded = event.succeeded;
              if (!event.succeeded) {
                result.error =
                    event.error.message.empty()
                        ? L"SQLite could not commit the deletion."
                        : Utf8ToWide(event.error.message);
              }
              OnStorageOperationReady(std::move(result));
            } else if constexpr (
                std::is_same_v<
                    Event,
                    feathercast::persistence::FileIndexWriteCompleted> ||
                std::is_same_v<
                    Event,
                    feathercast::persistence::ClipboardPruned>) {
              if (!event.succeeded) {
                ReportPersistenceFailure(
                    event.error.message.empty()
                        ? L"Local data could not be saved."
                        : Utf8ToWide(event.error.message));
              } else if constexpr (std::is_same_v<
                                       Event,
                                       feathercast::persistence::
                                           FileIndexWriteCompleted>) {
                if (visible_ &&
                    feathercast::search_scope::Parse(query_).scope ==
                        feathercast::search_scope::Scope::Files) {
                  RequestSearch();
                }
              }
            } else if constexpr (std::is_same_v<
                                     Event,
                                     feathercast::persistence::
                                         ClipboardStored>) {
              if (!event.entry) {
                ReportPersistenceFailure(
                    event.error.message.empty()
                        ? L"Clipboard history could not be saved."
                        : Utf8ToWide(event.error.message));
              }
            } else if constexpr (std::is_same_v<
                                     Event,
                                     feathercast::persistence::
                                         ClipboardLoaded>) {
              if (!event.error.message.empty()) {
                ReportPersistenceFailure(Utf8ToWide(event.error.message));
              } else if (settings_.clipboardHistoryEnabled) {
                std::vector<ClipboardEntry> loaded;
                unsigned long long serial = 0;
                for (const auto& stored : event.entries) {
                  ClipboardEntry item;
                  item.id = std::to_wstring(stored.id);
                  item.text = stored.text;
                  item.preview = stored.preview;
                  item.capturedAt = stored.capturedAt;
                  loaded.push_back(std::move(item));
                  serial = std::max<unsigned long long>(
                      serial, static_cast<unsigned long long>(stored.id));
                }
                {
                  std::lock_guard lock(dataMutex_);
                  clipboardHistory_ = std::move(loaded);
                  clipboardSerial_ = serial;
                }
                MarkSearchDataChanged();
                RequestSearch();
                InvalidateRect(hwnd_, nullptr, FALSE);
              }
            } else if constexpr (std::is_same_v<
                                     Event,
                                     feathercast::persistence::WorkerFailed>) {
              ReportPersistenceFailure(
                  L"The persistence worker failed during " +
                  event.operation + L".");
            }
          },
          std::move(queued));
    }
  }

  void ReportPersistenceFailure(const std::wstring& detail) {
    SetSettingsStatus(
        StatusSeverity::Error,
        detail.empty()
            ? L"FeatherCast could not save local changes. Check disk access and available space."
            : detail);
    if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) {
      ShowTrayNotification(L"FeatherCast Storage",
                           L"Some local changes could not be saved.");
    }
  }

  void ReportBackgroundFailure(const std::wstring& detail) {
    SetSettingsStatus(StatusSeverity::Error, detail);
    if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) {
      ShowTrayNotification(L"FeatherCast", detail);
    }
  }

  LRESULT VolumeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_GETOBJECT:
        if (static_cast<LONG>(lParam) == OBJID_CLIENT) {
          return feathercast::accessibility::HandleGetObject(this, hwnd, wParam,
                                                              lParam);
        }
        break;
      case WM_DWMCOMPOSITIONCHANGED:
      case WM_THEMECHANGED:
      case WM_SETTINGCHANGE:
      case WM_DISPLAYCHANGE:
        RefreshSystemPreferences();
        ApplyGlass(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_PAINT:
        PaintVolumeControl();
        return 0;
      case WM_SIZE:
        ResizeGlassSurface(volumeSurface_, hwnd, LOWORD(lParam),
                           HIWORD(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_DPICHANGED: {
        auto* rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResetTextFormats();
        DiscardGlassDevice();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_TIMER:
        if (wParam == TIMER_VOLUME_REFRESH) RefreshVolumeFromSystem();
        return 0;
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && volumeVisible_) {
          HideVolumeControl(false);
        }
        return 0;
      case WM_KILLFOCUS:
        if (volumeVisible_) HideVolumeControl(false);
        return 0;
      case WM_KEYDOWN:
        if (volumeClosing_) return 0;
        HandleVolumeKeyDown(static_cast<UINT>(wParam));
        return 0;
      case WM_LBUTTONDOWN: {
        if (volumeClosing_) return 0;
        const float scale = GetWindowScale(hwnd);
        const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;
        RECT client{};
        GetClientRect(hwnd, &client);
        const float width = static_cast<float>(client.right) / scale;
        if (PointInRect(VolumeTrackHitRect(width), x, y)) {
          volumeDragging_ = true;
          SetCapture(hwnd);
          SetVolumeFromTrack(x, width);
        }
        return 0;
      }
      case WM_MOUSEMOVE:
        if (volumeClosing_) return 0;
        if (volumeDragging_ && GetCapture() == hwnd) {
          const float scale = GetWindowScale(hwnd);
          RECT client{};
          GetClientRect(hwnd, &client);
          SetVolumeFromTrack(
              static_cast<float>(GET_X_LPARAM(lParam)) / scale,
              static_cast<float>(client.right) / scale);
        }
        return 0;
      case WM_LBUTTONUP:
        if (volumeDragging_) {
          const float scale = GetWindowScale(hwnd);
          RECT client{};
          GetClientRect(hwnd, &client);
          SetVolumeFromTrack(
              static_cast<float>(GET_X_LPARAM(lParam)) / scale,
              static_cast<float>(client.right) / scale);
          volumeDragging_ = false;
          if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
      case WM_CANCELMODE:
      case WM_CAPTURECHANGED:
        volumeDragging_ = false;
        return 0;
      case WM_CLOSE:
        HideVolumeControl(true);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  LRESULT SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_DESTROY:
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_GETOBJECT:
        if (static_cast<LONG>(lParam) == OBJID_CLIENT) {
          return feathercast::accessibility::HandleGetObject(this, hwnd, wParam, lParam);
        }
        break;
      case WM_DWMCOMPOSITIONCHANGED:
      case WM_THEMECHANGED:
      case WM_SETTINGCHANGE:
      case WM_DISPLAYCHANGE:
        RefreshSystemPreferences();
        ApplyGlass(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_PAINT:
        PaintSettings();
        return 0;
      case WM_SIZE:
        ResizeGlassSurface(settingsSurface_, settingsHwnd_, LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_DPICHANGED: {
        auto prc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ResetTextFormats();
        DiscardGlassDevice();
        // The visible corners are drawn by Direct2D; no window region is used.
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        const float scale = GetWindowScale(hwnd);
        const float lx = static_cast<float>(pt.x) / scale;
        const float ly = static_cast<float>(pt.y) / scale;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float lwidth = static_cast<float>(rc.right - rc.left) / scale;
        const bool overClose = lx >= lwidth - 46.0f && lx <= lwidth - 14.0f && ly >= 12.0f && ly <= 44.0f;
        if (ly >= 0.0f && ly < 52.0f && !overClose) return HTCAPTION;
        return HTCLIENT;
      }
      case WM_KEYDOWN:
        if (settingsClosing_) return 0;
        if (wParam == VK_ESCAPE && !recording_) {
          HideSettings();
        } else {
          HandleSettingsKeyDown(static_cast<UINT>(wParam));
        }
        return 0;
      case WM_MOUSEMOVE: {
        const float scale = GetWindowScale(hwnd);
        OnSettingsMouseMove(static_cast<float>(GET_X_LPARAM(lParam)) / scale, static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_MOUSEWHEEL:
        OnSettingsMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
      case WM_MOUSELEAVE:
        mouseTracking_ = false;
        if (settingsHover_ != -1) {
          settingsHover_ = -1;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_LBUTTONDOWN: {
        if (settingsClosing_) return 0;
        const float scale = GetWindowScale(hwnd);
        OnPointerDown(hwnd,
                      static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                      static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_LBUTTONUP: {
        if (settingsClosing_) return 0;
        const float scale = GetWindowScale(hwnd);
        OnPointerUp(hwnd,
                    static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                    static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_CANCELMODE:
      case WM_CAPTURECHANGED:
        CancelPointerPress(hwnd);
        return 0;
      case WM_CLOSE:
        HideSettings();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  bool InitializeFactories() {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
      return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory_)))) {
      dwriteFactory_.Reset();
      return false;
    }
    // The Direct3D/Direct2D/DirectComposition device stack is created lazily on first paint.
    return true;
  }

  bool RegisterWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.hInstance = instance_;
    wc.lpfnWndProc = StaticWndProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    wc.lpszClassName = kSettingsWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }

    wc.lpszClassName = kVolumeWindowClass;
    return RegisterClassExW(&wc) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }

  // (Re)applies the transparent window treatment. Direct2D keeps drawing the dark,
  // readable panel tint while DWM blurs the live desktop content behind it.
  void ApplyGlass(HWND hwnd) {
    ApplyDarkMode(hwnd);
    MARGINS margins{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    ApplyModernBackdrop(hwnd, DwmBackdropType::None);

    BOOL compositionEnabled = FALSE;
    const bool canBlur =
        !highContrast_ &&
        SUCCEEDED(DwmIsCompositionEnabled(&compositionEnabled)) &&
        compositionEnabled != FALSE;
    bool blurApplied = false;
    if (canBlur) {
      // ACCENT_ENABLE_ACRYLICBLURBEHIND falls back to a solid tint when the
      // Windows transparency-effects preference is disabled. The plain blur
      // state remains a live blur; FeatherCast supplies its own theme tint.
      blurApplied = ApplyAccentPolicy(hwnd, AccentState::EnableBlurBehind);
    } else {
      ApplyAccentPolicy(hwnd, AccentState::Disabled);
    }

    if (hwnd == settingsHwnd_) {
      settingsBlurApplied_ = blurApplied;
    } else if (hwnd == volumeHwnd_) {
      volumeBlurApplied_ = blurApplied;
    } else if (hwnd == hwnd_) {
      overlayBlurApplied_ = blurApplied;
    }
    ApplyDwmRoundedCorners(hwnd, blurApplied);
    DisableDwmBorder(hwnd);
  }

  bool CreateMainWindow() {
    const int width = OverlayWidth();
    hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
      kWindowClass,
      L"FeatherCast",
      WS_POPUP,
      -32000,
      -32000,
      width,
      WIN_HEIGHT,
      nullptr,
      nullptr,
      instance_,
      this);

    if (!hwnd_) return false;
    SetWindowPos(hwnd_, HWND_TOPMOST, -32000, -32000, width, WIN_HEIGHT, SWP_NOACTIVATE | SWP_HIDEWINDOW);
    // DirectComposition supplies per-pixel alpha over the DWM Acrylic backdrop.
    ApplyGlass(hwnd_);
    return true;
  }

  bool CreateSettingsWindow() {
    settingsHwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
      kSettingsWindowClass,
      L"FeatherCast Settings",
      WS_POPUP,
      -32000,
      -32000,
      SETTINGS_WIDTH,
      SETTINGS_HEIGHT,
      nullptr,
      nullptr,
      instance_,
      this);

    if (!settingsHwnd_) return false;
    // Settings uses the same transparent DirectComposition background as the launcher.
    ApplyGlass(settingsHwnd_);
    return true;
  }

  bool CreateVolumeWindow() {
    volumeHwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        kVolumeWindowClass, L"FeatherCast Volume Control", WS_POPUP, -32000,
        -32000, VOLUME_WIDTH, VOLUME_HEIGHT, nullptr, nullptr, instance_, this);
    if (!volumeHwnd_) return false;
    ApplyGlass(volumeHwnd_);
    return true;
  }

  void ReleaseComResources() {
    DiscardGlassDevice();
    ResetTextFormats();
    wicFactory_.Reset();
    dwriteFactory_.Reset();
  }

  // Per-window composition render surface: a DXGI swap chain bound to a Direct2D device
  // context and shown through a DirectComposition visual. Renders with per-pixel alpha so
  // desktop content behind the popup shows through transparent pixels.
  struct GlassSurface {
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bitmap;
    void Reset() {
      if (dc) dc->SetTarget(nullptr);
      bitmap.Reset();
      visual.Reset();
      target.Reset();
      swapChain.Reset();
      dc.Reset();
    }
  };

  // Builds the shared Direct3D 11 / Direct2D / DirectComposition device stack. Direct2D
  // renders into a DirectComposition swap chain with premultiplied alpha instead of an
  // opaque HWND surface, so transparent pixels let desktop content show through.
  bool EnsureGlassDevice() {
    if (d2dDevice_) return true;
    // A previous partial initialization must never be reused or overwritten.
    dcompDevice_.Reset();
    dxgiFactory_.Reset();
    d2dDevice_.Reset();
    d3dDevice_.Reset();

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = E_FAIL;
    if (!forceWarp_) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                            d3dDevice_.GetAddressOf(), nullptr, nullptr);
    }
    if (FAILED(hr)) {
      // Fall back to WARP so the app still renders on machines without a usable GPU path.
      d3dDevice_.Reset();
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                             levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                             d3dDevice_.GetAddressOf(), nullptr, nullptr);
    }
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3dDevice_.As(&dxgiDevice))) return false;
    if (FAILED(D2D1CreateDevice(dxgiDevice.Get(), nullptr, d2dDevice_.GetAddressOf()))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return false;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(dxgiFactory_.GetAddressOf())))) return false;
    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(dcompDevice_.GetAddressOf())))) return false;
    return true;
  }

  // Tears down the whole device stack (used on device-lost). It is rebuilt lazily.
  void DiscardGlassDevice() {
    overlaySurface_.Reset();
    settingsSurface_.Reset();
    volumeSurface_.Reset();
    dcompDevice_.Reset();
    dxgiFactory_.Reset();
    d2dDevice_.Reset();
    d3dDevice_.Reset();
    ClearIconBitmaps();
    previewBitmap_.Reset();
    brushCache_.clear();
  }

  static bool IsDeviceLoss(HRESULT result) {
    return result == D2DERR_RECREATE_TARGET ||
           result == DXGI_ERROR_DEVICE_REMOVED ||
           result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG ||
           result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
  }

  void ScheduleRenderRecovery(const wchar_t* phase, HRESULT result) {
    std::wstring detail = L"phase=" + std::wstring(phase) + L" hr=0x" +
                          ToHex(static_cast<unsigned>(result));
    if (d3dDevice_) {
      const HRESULT removed = d3dDevice_->GetDeviceRemovedReason();
      detail += L" removed=0x" + ToHex(static_cast<unsigned>(removed));
    }
    DebugGraphicsLog(detail);
    if (IsDeviceLoss(result) && ++deviceLossCount_ >= 2) forceWarp_ = true;
    DiscardGlassDevice();
    if (!renderRecoveryQueued_ && hwnd_) {
      renderRecoveryQueued_ =
          PostMessageW(hwnd_, WM_RENDER_RECOVER, 0, 0) != FALSE;
    }
  }

  // (Re)binds the swap chain's back buffer as the Direct2D device context's target.
  HRESULT BindSurfaceTarget(GlassSurface& surface, float dpi) {
    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT result = surface.swapChain->GetBuffer(
        0, IID_PPV_ARGS(dxgiSurface.GetAddressOf()));
    if (FAILED(result)) return result;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    surface.bitmap.Reset();
    result = surface.dc->CreateBitmapFromDxgiSurface(
        dxgiSurface.Get(), &props, surface.bitmap.GetAddressOf());
    if (FAILED(result)) return result;
    surface.dc->SetTarget(surface.bitmap.Get());
    surface.dc->SetDpi(dpi, dpi);
    return S_OK;
  }

  // Creates the composition swap chain, Direct2D context and DComp visual for a window.
  bool CreateGlassSurface(GlassSurface& surface, HWND hwnd) {
    if (surface.dc) return true;
    if (!EnsureGlassDevice()) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT width = std::max<UINT>(1, rc.right - rc.left);
    const UINT height = std::max<UINT>(1, rc.bottom - rc.top);
    const float dpi = GetWindowScale(hwnd) * 96.0f;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory_->CreateSwapChainForComposition(d3dDevice_.Get(), &desc, nullptr,
                                                           surface.swapChain.GetAddressOf()))) {
      surface.Reset();
      return false;
    }
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, surface.dc.GetAddressOf())) ||
        FAILED(dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, surface.target.GetAddressOf())) ||
        FAILED(dcompDevice_->CreateVisual(surface.visual.GetAddressOf()))) {
      surface.Reset();
      return false;
    }
    surface.visual->SetContent(surface.swapChain.Get());
    surface.target->SetRoot(surface.visual.Get());
    dcompDevice_->Commit();

    if (FAILED(BindSurfaceTarget(surface, dpi))) {
      surface.Reset();
      return false;
    }
    EnsureTextFormats();
    return true;
  }

  // Create and render a surface before its HWND becomes visible. The first
  // Direct3D/DirectComposition setup and DirectWrite layout pass can be much
  // slower than a normal frame; keeping that work outside the reveal clock
  // prevents the first opening from jumping ahead while later openings are
  // already smooth from cached resources.
  bool PrewarmGlassSurface(HWND hwnd, GlassSurface& surface) {
    if (!hwnd) return false;
    if (surface.dc) return true;
    if (!CreateGlassSurface(surface, hwnd)) return false;
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    return surface.dc != nullptr;
  }

  // Resizes a surface's swap chain to match the window's new physical client size.
  void ResizeGlassSurface(GlassSurface& surface, HWND hwnd, UINT width, UINT height) {
    if (!surface.swapChain || !surface.dc) return;
    width = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    surface.dc->SetTarget(nullptr);
    surface.bitmap.Reset();
    const HRESULT resizeResult = surface.swapChain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(resizeResult)) {
      surfaceResizeFailed_ = true;
      ScheduleRenderRecovery(L"resize-buffers", resizeResult);
      return;
    }
    const HRESULT bindResult =
        BindSurfaceTarget(surface, GetWindowScale(hwnd) * 96.0f);
    if (FAILED(bindResult)) {
      surfaceResizeFailed_ = true;
      ScheduleRenderRecovery(L"bind-target", bindResult);
    }
  }

  void ResetTextFormats() {
    inputFormat_.Reset();
    rowFormat_.Reset();
    calculationResultFormat_.Reset();
    subFormat_.Reset();
    sectionFormat_.Reset();
    footerFormat_.Reset();
    footerRightFormat_.Reset();
    titleFormat_.Reset();
    labelFormat_.Reset();
    bodyFormat_.Reset();
    buttonFormat_.Reset();
    centerFormat_.Reset();
    emojiFormat_.Reset();
    volumeValueFormat_.Reset();
  }

  static feathercast::theme::Color ThemeColorFromSystem(COLORREF color) {
    return {
      GetRValue(color) / 255.0f,
      GetGValue(color) / 255.0f,
      GetBValue(color) / 255.0f,
      1.0f,
    };
  }

  void RefreshSystemPreferences() {
    theme_ = feathercast::theme::LoadTheme(ThemePath());
    HIGHCONTRASTW contrast{sizeof(contrast)};
    highContrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                    (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    BOOL animations = TRUE;
    systemAnimationsEnabled_ =
        !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) || animations != FALSE;
    if (highContrast_) {
      theme_.overlayBackground = ThemeColorFromSystem(GetSysColor(COLOR_WINDOW));
      theme_.settingsBackground = theme_.overlayBackground;
      theme_.surface = ThemeColorFromSystem(GetSysColor(COLOR_BTNFACE));
      theme_.surfaceHover = ThemeColorFromSystem(GetSysColor(COLOR_HIGHLIGHT));
      theme_.selectedBase = ThemeColorFromSystem(GetSysColor(COLOR_HIGHLIGHT));
      theme_.iconTile = theme_.surface;
      theme_.border = ThemeColorFromSystem(GetSysColor(COLOR_WINDOWTEXT));
      theme_.divider = theme_.border;
      theme_.textPrimary = ThemeColorFromSystem(GetSysColor(COLOR_WINDOWTEXT));
      theme_.textMuted = ThemeColorFromSystem(GetSysColor(COLOR_GRAYTEXT));
      theme_.textDim = theme_.textMuted;
      theme_.sectionText = theme_.textPrimary;
      theme_.accentFallback = ThemeColorFromSystem(GetSysColor(COLOR_HIGHLIGHT));
    }
    // Theme, high-contrast and display preference changes invalidate every
    // device-bound surface as well as all DirectWrite formats.
    ResetTextFormats();
    DiscardGlassDevice();
    if (!FadeAnimationsAllowed()) {
      SnapAllMotionToTargets();
    } else if (!SpatialAnimationsAllowed()) {
      SnapSpatialMotionToTargets();
    }
  }

  bool AnimationAllowed(AnimationKind kind) const {
    return feathercast::settings::AnimationAllowed(
        settings_.animationLevel, kind, systemAnimationsEnabled_, highContrast_);
  }

  bool FadeAnimationsAllowed() const {
    return AnimationAllowed(AnimationKind::Fade);
  }

  bool SpatialAnimationsAllowed() const {
    return AnimationAllowed(AnimationKind::Spatial);
  }

  bool ControlAnimationsAllowed() const {
    return AnimationAllowed(AnimationKind::ControlFeedback);
  }

  void EnsureTextFormats() {
    if (!dwriteFactory_ || inputFormat_) return;
    // 18px Regular: elegant search-bar input (was 19px).
    CreateTextFormat(18.0f, DWRITE_FONT_WEIGHT_NORMAL, inputFormat_);
    // 14px Medium: stronger name/title separation from subtitle (was 15px Normal).
    CreateTextFormat(14.0f, DWRITE_FONT_WEIGHT_MEDIUM, rowFormat_);
    CreateTextFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                     calculationResultFormat_);
    if (calculationResultFormat_) {
      calculationResultFormat_->SetTextAlignment(
          DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, subFormat_);
    // 10px Semi-Bold: section labels in all-caps feel (was 11px).
    CreateTextFormat(10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, sectionFormat_);
    CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, footerFormat_);
    CreateTextFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, footerRightFormat_);
    if (footerRightFormat_) {
      footerRightFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    CreateTextFormat(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_);
    CreateTextFormat(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, labelFormat_);
    CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_);
    CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonFormat_);
    CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, centerFormat_);
    if (centerFormat_) {
      centerFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      centerFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    CreateTextFormat(22.0f, DWRITE_FONT_WEIGHT_NORMAL, emojiFormat_);
    if (emojiFormat_) {
      emojiFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      emojiFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    CreateTextFormat(28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                     volumeValueFormat_);
    if (volumeValueFormat_) {
      volumeValueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
  }

  void CreateTextFormat(float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
    HRESULT hr = dwriteFactory_->CreateTextFormat(
      theme_.fontFamily.c_str(),
      nullptr,
      weight,
      DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL,
      size,
      L"",
      out.GetAddressOf());
    if (FAILED(hr) && theme_.fontFamily != L"Segoe UI") {
      out.Reset();
      dwriteFactory_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size,
        L"",
        out.GetAddressOf());
    }
    if (out) {
      out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
  }

  bool CreateTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    wcscpy_s(nid.szTip, L"FeatherCast");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
      if (nid.hIcon) DestroyIcon(nid.hIcon);
      return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    tray_ = nid;
    return true;
  }

  void RemoveTray() {
    if (tray_.cbSize) {
      Shell_NotifyIconW(NIM_DELETE, &tray_);
      if (tray_.hIcon) DestroyIcon(tray_.hIcon);
      tray_ = {};
    }
  }

  bool InstallHook() {
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, StaticKeyboardProc, GetModuleHandleW(nullptr), 0);
    return hook_ != nullptr;
  }

  void SetHookModifier(UINT vk, bool pressed) {
    const UINT generic = GenericModifier(vk);
    if (generic == VK_CONTROL) hookModifiers_.ctrl = pressed;
    else if (generic == VK_MENU) hookModifiers_.alt = pressed;
    else if (generic == VK_SHIFT) hookModifiers_.shift = pressed;
    else if (generic == VK_LWIN) hookModifiers_.win = pressed;
  }

  bool RegisterShortcutHotKey() {
    UnregisterShortcutHotKey();
    if (IsExclusiveWinShortcut(shortcut_)) {
      // A bare Windows-key shortcut is handled on key-up by the low-level
      // hook. Reserving VK_LWIN/VK_RWIN with RegisterHotKey interferes with
      // native Windows chords such as Win+Arrow.
      return hook_ != nullptr;
    }
    const auto hotKey = ToHotKeySpec(shortcut_);
    if (!hwnd_ || !hotKey.supported) return false;
    hotKeyRegistered_ = RegisterHotKey(hwnd_, HOTKEY_OPEN_SEARCH, hotKey.modifiers, hotKey.vk) != FALSE;
    return hotKeyRegistered_;
  }

  bool EnsureRenderResources() {
    EnsureTextFormats();
    return inputFormat_ && rowFormat_ && calculationResultFormat_ &&
           subFormat_ && sectionFormat_ && footerFormat_ &&
           footerRightFormat_ && titleFormat_ && labelFormat_ && bodyFormat_ &&
           buttonFormat_ && centerFormat_ && emojiFormat_ &&
           volumeValueFormat_;
  }

  bool CanActivateShortcut(const ShortcutSpec& candidate) const {
    if (candidate.display.empty() || candidate.display == L"none") return true;
    if (IsExclusiveWinShortcut(candidate)) {
      return hook_ != nullptr;
    }
    const auto hotKey = ToHotKeySpec(candidate);
    if (!hotKey.supported) return hook_ != nullptr;
    if (!hwnd_) return false;
    const bool registered =
        RegisterHotKey(hwnd_, HOTKEY_VALIDATE_SHORTCUT, hotKey.modifiers, hotKey.vk) != FALSE;
    if (registered) UnregisterHotKey(hwnd_, HOTKEY_VALIDATE_SHORTCUT);
    return registered;
  }

  void UnregisterShortcutHotKey() {
    if (!hotKeyRegistered_) return;
    if (hwnd_) {
      UnregisterHotKey(hwnd_, HOTKEY_OPEN_SEARCH);
    }
    hotKeyRegistered_ = false;
  }

  LRESULT HandleRecordingKey(UINT vk, bool down, bool up) {
    const auto result = shortcutRecorder_.Handle(vk, down, up);
    if (result.canceled) {
      feathercast::ui::SettingsController::CancelShortcutRecording(
          settingsState_);
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    } else if (result.done) {
      feathercast::ui::SettingsController::SetPendingShortcut(
          settingsState_, result.shortcut);
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    }
    return result.consume ? 1 : 0;
  }

  size_t ClipboardHistoryLimit() const {
    return static_cast<size_t>(std::clamp(settings_.clipboardHistoryLimit,
                                         MIN_CLIPBOARD_HISTORY_LIMIT,
                                         MAX_CLIPBOARD_HISTORY_LIMIT));
  }

  void ApplyClipboardHistoryLimit() {
    const size_t limit = ClipboardHistoryLimit();
    {
      std::lock_guard lock(dataMutex_);
      if (clipboardHistory_.size() > limit) clipboardHistory_.resize(limit);
    }
    if (!persistence_.PruneClipboard(limit)) {
      ReportPersistenceFailure(L"The persistence worker is unavailable.");
    }
    MarkSearchDataChanged();
    RequestSearch();
  }

  size_t FileIndexLimit() const {
    return static_cast<size_t>(std::clamp(settings_.fileIndexMaxEntries,
                                         MIN_FILE_INDEX_ENTRIES,
                                         MAX_FILE_INDEX_ENTRIES));
  }

  void PromptForPrivacyConsentIfNeeded() {
    if (settings_.privacyConsentVersion >= 1 || !hadLegacyOperationalData_) return;
    const int choice = MessageBoxW(
        hwnd_,
        L"FeatherCast previously stored clipboard text and indexed personal file paths.\n\n"
        L"Enable Clipboard History and Files & Folders indexing? You can change these "
        L"independently in Settings and clear their data at any time.",
        L"FeatherCast Privacy",
        MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2);
    settings_.privacyConsentVersion = 1;
    settings_.clipboardHistoryEnabled = choice == IDYES;
    settings_.fileIndexEnabled = choice == IDYES;
    PersistSettings();
  }

  void UpdateClipboardListenerRegistration() {
    const bool shouldListen = settings_.privacyConsentVersion >= 1 && settings_.clipboardHistoryEnabled;
    if (shouldListen && !clipboardListenerRegistered_ && hwnd_) {
      clipboardListenerRegistered_ = AddClipboardFormatListener(hwnd_) != FALSE;
    } else if (!shouldListen && clipboardListenerRegistered_ && hwnd_) {
      RemoveClipboardFormatListener(hwnd_);
      clipboardListenerRegistered_ = false;
    }
  }

  void ShowTrayNotification(const std::wstring& title, const std::wstring& message) {
    if (!tray_.cbSize) return;
    NOTIFYICONDATAW notification = tray_;
    notification.uFlags = NIF_INFO;
    wcsncpy_s(notification.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(notification.szInfo, message.c_str(), _TRUNCATE);
    notification.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &notification);
  }

  void LoadPersistentState() {
    const auto state = persistence_.LoadStorageForStartup(
        FileIndexLimit(), ClipboardHistoryLimit(), false);
    if (!state.opened) {
      const auto& error = state.error;
      const std::wstring detail =
          error.message.empty() ? L"Unknown SQLite error." : Utf8ToWide(error.message);
      MessageBoxW(hwnd_,
                  (L"FeatherCast could not open its local database.\n\n" + detail +
                   L"\n\nFile indexing and clipboard persistence will be unavailable.").c_str(),
                  L"FeatherCast Storage", MB_OK | MB_ICONWARNING);
      return;
    }
    if (state.recoveredFromCorruption) {
      MessageBoxW(
          hwnd_,
          (L"FeatherCast detected a damaged local database and created a new one.\n\n"
           L"The damaged file was preserved at:\n" +
           state.quarantinedPath.wstring()).c_str(),
          L"FeatherCast Storage Recovery", MB_OK | MB_ICONWARNING);
    }

    if (settings_.clipboardHistoryEnabled) {
      for (const auto& entry : state.clipboard) {
        ClipboardEntry item;
        item.id = std::to_wstring(entry.id);
        item.text = entry.text;
        item.preview = entry.preview;
        item.capturedAt = entry.capturedAt;
        clipboardHistory_.push_back(std::move(item));
        clipboardSerial_ = std::max<unsigned long long>(
            clipboardSerial_, static_cast<unsigned long long>(entry.id));
      }
    }
  }

  void ReloadTheme() {
    theme_ = feathercast::theme::LoadTheme(ThemePath());
    RefreshSystemPreferences();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (settingsHwnd_) InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void StartAppDiscovery(bool skipFileIndexOnce = false) {
    const uint64_t generation = ++discoveryGeneration_;
    appsReady_.store(false, std::memory_order_release);
    PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
    if (!skipFileIndexOnce && settingsHwnd_ && IsWindowVisible(settingsHwnd_) &&
        settings_.fileIndexEnabled &&
        (!settingsStatus_ ||
         settingsStatus_->severity != StatusSeverity::Error)) {
      SetSettingsStatus(StatusSeverity::Progress,
                        L"Refreshing apps and file index...");
    }

    DiscoveryRequest request;
    request.generation = generation;
    if (!discoveryService_.Refresh(std::move(request))) {
      ReportBackgroundFailure(L"The discovery worker is unavailable.");
    }
    // File indexing is intentionally demand-loaded.  Starting a full crawl
    // here makes an otherwise idle launcher scan the user's folders at every
    // process start; ConfigureFileIndex() is called when the Files scope or a
    // privacy/rebuild action explicitly needs it.
  }

  std::vector<std::wstring> ResolvedFileIndexRoots() const {
    if (!settings_.fileIndexRoots.empty()) return settings_.fileIndexRoots;
    std::vector<std::wstring> roots;
    for (const KNOWNFOLDERID& folder :
         {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads}) {
      const auto path = KnownFolderPath(folder);
      if (!path.empty()) roots.push_back(path);
    }
    return roots;
  }

  void ConfigureFileIndex() {
    if (!settings_.fileIndexEnabled) {
      StopFileIndexService();
      return;
    }
    if (!fileIndexServiceStarted_) {
      fileIndexService_.Start();
      fileIndexServiceStarted_ = true;
    }
    feathercast::files::IndexRequest request;
    request.generation = ++fileIndexGeneration_;
    request.limit = FileIndexLimit();
    request.contentEnabled = settings_.fileContentIndexEnabled;
    if (settings_.fileIndexEnabled) request.roots = ResolvedFileIndexRoots();
    if (!fileIndexService_.Reconfigure(std::move(request))) {
      ReportBackgroundFailure(L"The live file index worker is unavailable.");
    }
    fileIndexConfigured_ = true;
  }

  void StopFileIndexService() {
    if (!fileIndexServiceStarted_) return;
    fileIndexService_.Stop();
    fileIndexServiceStarted_ = false;
    fileIndexConfigured_ = false;
  }

  bool EnsureFileIndexLoaded() {
    if (!settings_.fileIndexEnabled || fileIndexLoaded_) return fileIndexLoaded_;
    const auto entries = persistence_.LoadFileIndex(FileIndexLimit());
    std::vector<AppEntry> files;
    files.reserve(entries.size());
    for (const auto& entry : entries) {
      if (!entry.path.empty() && !entry.name.empty()) files.push_back(FileIndexApp(entry));
    }
    {
      std::lock_guard lock(dataMutex_);
      fileIndex_ = files;
    }
    fileSearchService_.UpdateFiles(std::move(files));
    fileIndexLoaded_ = true;
    MarkSearchDataChanged();
    return true;
  }

  void OnFileIndexReady(feathercast::files::IndexStatus status) {
    if (!fileIndexServiceStarted_ ||
        !fileIndexService_.IsCurrent(status.generation)) return;
    const auto entryCount = status.entries.size();
    std::vector<AppEntry> files;
    files.reserve(status.entries.size());
    for (const auto& entry : status.entries) {
      if (!entry.path.empty() && !entry.name.empty()) {
        files.push_back(FileIndexApp(entry));
      }
    }
    if (!persistence_.UpdateFileIndex(std::move(status.entries))) {
      ReportPersistenceFailure(L"The persistence worker is unavailable.");
    }
    {
      std::lock_guard lock(dataMutex_);
      fileIndex_ = files;
    }
    fileSearchService_.UpdateFiles(std::move(files));
    fileIndexStatus_ = std::move(status);
    KillTimer(hwnd_, TIMER_FILE_SNAPSHOT);
    SetTimer(hwnd_, TIMER_FILE_SNAPSHOT, 500, nullptr);
    if (visible_ && feathercast::search_scope::Parse(query_).scope ==
                        feathercast::search_scope::Scope::Files) {
      RequestSearch();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (settingsHwnd_ && IsWindowVisible(settingsHwnd_)) {
      SetSettingsStatus(
          fileIndexStatus_->live ? StatusSeverity::Success
                                 : StatusSeverity::Error,
          fileIndexStatus_->message + L" " + std::to_wstring(entryCount) +
              L" entries, " +
              std::to_wstring(fileIndexStatus_->indexedContentFiles) +
              L" content files.");
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    }
  }

  bool DiscoveryCanceled(std::stop_token stopToken, uint64_t generation) const {
    return stopToken.stop_requested() || stopThreads_ ||
           !discoveryService_.IsCurrent(generation);
  }

  std::optional<DiscoveryResult> PerformDiscovery(
      const DiscoveryRequest& request, std::stop_token stopToken) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ScopeExit uninitialize([] { CoUninitialize(); });
    auto apps = DiscoverApps(stopToken, request.generation);
    if (!apps || DiscoveryCanceled(stopToken, request.generation)) {
      return std::nullopt;
    }

    DiscoveryResult result;
    result.generation = request.generation;
    result.apps = std::move(*apps);
    return result;
  }

  void OnDiscoveryCompleted(DiscoveryResult result) {
    if (!discoveryService_.IsCurrent(result.generation)) return;
    {
      std::lock_guard lock(dataMutex_);
      apps_ = std::move(result.apps);
    }
    appsReady_.store(true, std::memory_order_release);
    MarkSearchDataChanged();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (settingsStatus_ &&
        settingsStatus_->severity == StatusSeverity::Progress &&
        settingsStatus_->text.find(L"index") != std::wstring::npos) {
      SetSettingsStatus(StatusSeverity::Success,
                        L"Apps refreshed; file indexing continues in the background.");
    }
  }

  // Loads cached FX rates immediately, then refreshes from the network in the
  // background if the cache is stale or missing.
  void StartCurrencyFetch() {
    CurrencyRates cached = LoadCurrencyCache();
    {
      std::lock_guard lock(dataMutex_);
      currencyRates_ = cached;
    }
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!cached.perUsd.empty() && (now - cached.fetchedAt) < 12 * 3600) return;

    currencyService_.Run([this](std::stop_token stopToken)
                             -> std::optional<CurrencyRates> {
      if (stopToken.stop_requested() || stopThreads_) return std::nullopt;
      const auto body = feathercast::network::HttpsGet(
          L"open.er-api.com", L"/v6/latest/USD");
      if (!body || stopToken.stop_requested() || stopThreads_) {
        return std::nullopt;
      }
      auto rates = RatesFromJson(*body);
      if (rates.empty()) return std::nullopt;
      CurrencyRates updated;
      updated.perUsd = std::move(rates);
      updated.fetchedAt = static_cast<long long>(std::time(nullptr));
      SaveCurrencyCache(updated);
      return updated;
    });
  }

  void StartAutomaticUpdateCheck() {
    if (!settings_.updateChecksEnabled) return;
    const long long now = UnixNow();
    if (settings_.lastUpdateAttempt > 0 && now - settings_.lastUpdateAttempt < 3600) return;
    if (settings_.lastUpdateCheck > 0 && now - settings_.lastUpdateCheck < 24 * 3600) return;
    StartUpdateCheck(false);
  }

  void StartUpdateCheck(bool manual) {
    if (updateService_.Running()) {
      if (manual) {
        MessageBoxW(hwnd_, L"An update check is already running.", L"FeatherCast Updates",
                    MB_OK | MB_ICONINFORMATION);
      }
      return;
    }

    settings_.lastUpdateAttempt = UnixNow();
    PersistSettings();
    const std::wstring dismissedVersion = settings_.dismissedUpdateVersion;

    AppendUpdateLog(manual ? L"Manual update check started" : L"Automatic update check started");
    updateService_.Run([this, manual, dismissedVersion](std::stop_token stopToken)
                           -> std::optional<UpdateTaskResult> {
      UpdateTaskResult result;
      result.manual = manual;
      result.kind = UpdateTaskKind::Check;

      const auto body = feathercast::network::HttpsGet(
          L"api.github.com",
          L"/repos/GenericLeon0/FeatherCast/releases/latest");
      if (stopToken.stop_requested() || stopThreads_) {
        return std::nullopt;
      }
      if (!body) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Unable to reach GitHub Releases.";
        return result;
      }

      auto release = feathercast::updater::ParseGitHubReleaseJson(*body);
      if (!release) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"GitHub returned update metadata FeatherCast could not parse.";
        return result;
      }

      if (!feathercast::updater::IsEligibleRelease(*release, kFeatherCastVersion)) {
        result.status = UpdateTaskStatus::NoUpdate;
        result.message = L"FeatherCast is up to date.";
        return result;
      }

      if (!manual && !dismissedVersion.empty() && Lower(release->tagName) == Lower(dismissedVersion)) {
        result.status = UpdateTaskStatus::NoUpdate;
        result.message = L"Newest release was already dismissed: " + release->tagName;
        return result;
      }

      const auto installer = feathercast::updater::SelectInstallerAsset(*release);
      const auto hash = installer ? feathercast::updater::SelectSha256Asset(*release, *installer) : std::nullopt;
      if (!installer || !hash) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Latest release is missing the required installer or SHA-256 asset.";
        result.release = std::move(*release);
        return result;
      }

      result.status = UpdateTaskStatus::Available;
      result.message = L"Update available: " + release->tagName;
      result.release = std::move(*release);
      return result;
    });
  }

  void StartUpdateDownload(feathercast::updater::ReleaseInfo release, bool manual) {
    if (updateService_.Running()) {
      MessageBoxW(hwnd_, L"Another update task is already running.", L"FeatherCast Updates",
                  MB_OK | MB_ICONINFORMATION);
      return;
    }
    if (feathercast::updater::ParseSignerThumbprints(
            kFeatherCastAllowedSignerThumbprints).empty()) {
      const int choice = MessageBoxW(
          hwnd_,
          L"This build does not contain a pinned FeatherCast signing certificate, "
          L"so it cannot safely install updates.\n\nOpen the GitHub release page instead?",
          L"FeatherCast Updates",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1);
      if (choice == IDYES && !release.htmlUrl.empty()) {
        ShellExecuteW(nullptr, L"open", release.htmlUrl.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
      }
      return;
    }

    AppendUpdateLog(L"Update download started: " + release.tagName);
    updateService_.Run(
        [this, release = std::move(release), manual](std::stop_token stopToken)
            mutable -> std::optional<UpdateTaskResult> {
      UpdateTaskResult result;
      result.manual = manual;
      result.kind = UpdateTaskKind::DownloadInstall;
      result.release = release;

      const auto installer = feathercast::updater::SelectInstallerAsset(release);
      const auto hash = installer ? feathercast::updater::SelectSha256Asset(release, *installer) : std::nullopt;
      if (!installer || !hash) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Update release is missing the required installer or SHA-256 asset.";
        return result;
      }

      const auto hashText = feathercast::network::HttpsGetUrl(
          hash->browserDownloadUrl, 64 * 1024);
      if (stopToken.stop_requested() || stopThreads_) {
        return std::nullopt;
      }
      if (!hashText || !feathercast::updater::ExtractSha256Hex(*hashText)) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Unable to download or parse the update SHA-256 file.";
        return result;
      }

      std::wstring fileName = std::filesystem::path(installer->name).filename().wstring();
      if (fileName.empty()) fileName = feathercast::updater::ExpectedInstallerAssetName(release.tagName);
      const auto installerPath = UpdatesPath() / fileName;
      if (!feathercast::network::HttpsDownloadToFile(
              installer->browserDownloadUrl, installerPath, stopToken)) {
        if (stopToken.stop_requested() || stopThreads_) {
          return std::nullopt;
        }
        result.status = UpdateTaskStatus::Error;
        result.message = L"Unable to download the update installer.";
        return result;
      }

      if (!feathercast::updater::VerifyFileSha256(installerPath, *hashText)) {
        std::error_code ec;
        std::filesystem::remove(installerPath, ec);
        result.status = UpdateTaskStatus::Error;
        result.message = L"Downloaded installer failed SHA-256 verification.";
        return result;
      }
      if (!feathercast::updater::VerifyAuthenticodeSigner(
              installerPath, kFeatherCastExpectedPublisher,
              kFeatherCastAllowedSignerThumbprints)) {
        std::error_code ec;
        std::filesystem::remove(installerPath, ec);
        result.status = UpdateTaskStatus::Error;
        result.message = L"Downloaded installer has an invalid or unexpected Authenticode signature.";
        return result;
      }

      result.status = UpdateTaskStatus::ReadyToInstall;
      result.installerPath = installerPath;
      result.message = L"Verified update installer: " + installerPath.wstring();
      return result;
    });
  }

  void OnUpdateReady(UpdateTaskResult result) {
    AppendUpdateLog(result.message);
    if (result.kind == UpdateTaskKind::Check && result.status != UpdateTaskStatus::Error) {
      settings_.lastUpdateCheck = UnixNow();
      PersistSettings();
    }

    if (result.kind == UpdateTaskKind::Check) {
      if (result.status == UpdateTaskStatus::NoUpdate) {
        if (result.manual) {
          const std::wstring message = L"FeatherCast " + std::wstring(kFeatherCastVersion) + L" is up to date.";
          MessageBoxW(hwnd_, message.c_str(), L"FeatherCast Updates", MB_OK | MB_ICONINFORMATION);
        }
        return;
      }
      if (result.status == UpdateTaskStatus::Error) {
        if (result.manual) {
          MessageBoxW(hwnd_, result.message.c_str(), L"FeatherCast Updates", MB_OK | MB_ICONWARNING);
        }
        return;
      }
      if (result.status == UpdateTaskStatus::Available) {
        const std::wstring releaseVersion = feathercast::updater::AssetVersionFromTag(result.release.tagName);
        if (!result.manual) {
          ShowTrayNotification(L"FeatherCast Update Available",
                               L"Version " + releaseVersion + L" is available. "
                               L"Use Check for Updates when you are ready.");
          return;
        }
        const std::wstring message =
            L"FeatherCast " + releaseVersion + L" is available.\n\nCurrent version: " +
            std::wstring(kFeatherCastVersion) + L"\n\nDownload, verify, and install it now?";
        const int choice = MessageBoxW(hwnd_, message.c_str(), L"FeatherCast Updates",
                                       MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (choice == IDYES) {
          settings_.dismissedUpdateVersion.clear();
          PersistSettings();
          StartUpdateDownload(std::move(result.release), result.manual);
        } else if (!result.manual) {
          settings_.dismissedUpdateVersion = result.release.tagName;
          PersistSettings();
        }
        return;
      }
    }

    if (result.kind == UpdateTaskKind::DownloadInstall) {
      if (result.status == UpdateTaskStatus::ReadyToInstall) {
        const std::wstring releaseVersion = feathercast::updater::AssetVersionFromTag(result.release.tagName);
        const std::wstring message =
            L"The FeatherCast " + releaseVersion +
            L" installer was downloaded and verified.\n\nInstall it now? FeatherCast will close.";
        const int choice = MessageBoxW(hwnd_, message.c_str(), L"FeatherCast Updates",
                                       MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (choice != IDYES) return;

        HINSTANCE launched = ShellExecuteW(nullptr, L"open", result.installerPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32) {
          MessageBoxW(hwnd_, L"FeatherCast could not start the verified installer.", L"FeatherCast Updates",
                      MB_OK | MB_ICONWARNING);
          return;
        }
        DestroyWindow(hwnd_);
        return;
      }

      MessageBoxW(hwnd_, result.message.c_str(), L"FeatherCast Updates", MB_OK | MB_ICONWARNING);
    }
  }

  std::optional<std::vector<AppEntry>> DiscoverApps(std::stop_token stopToken,
                                                    uint64_t generation) {
    std::vector<AppEntry> apps;
    std::map<std::wstring, size_t> identities;

    auto canonicalPathKey = [](const std::filesystem::path& path) {
      if (path.empty()) return std::wstring{};
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(path, ec);
      std::wstring value = Lower((ec ? path.lexically_normal() : canonical).wstring());
      while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
      }
      return value;
    };

    auto pathIsInside = [](const std::wstring& child,
                           const std::wstring& directory) {
      if (child.size() <= directory.size() ||
          child.compare(0, directory.size(), directory) != 0) {
        return false;
      }
      return child[directory.size()] == L'\\' || child[directory.size()] == L'/';
    };

    auto addUnique = [&](AppEntry entry) {
      if (entry.id.empty() || entry.name.empty()) return;
      std::vector<std::wstring> candidateIdentities;
      if (!entry.appUserModelId.empty()) {
        candidateIdentities.push_back(L"aumid:" + Lower(entry.appUserModelId));
      }
      if (!entry.targetPath.empty()) {
        candidateIdentities.push_back(L"target:" + Lower(entry.targetPath));
      }
      if (entry.isGame && !entry.path.empty()) {
        candidateIdentities.push_back(
            L"install:" + canonicalPathKey(entry.path));
      }
      if (!entry.isGame && !entry.launchTarget.empty()) {
        candidateIdentities.push_back(L"launch:" + Lower(entry.launchTarget));
      }
      candidateIdentities.push_back(L"id:" + Lower(entry.id));

      std::optional<size_t> foundIndex;
      for (const auto& identity : candidateIdentities) {
        if (const auto found = identities.find(identity);
            found != identities.end()) {
          foundIndex = found->second;
          break;
        }
      }

      // A provider usually knows the installation root while a Start Menu
      // shortcut knows the concrete game executable. Correlate those without
      // using directory identity for ordinary apps, which could collapse two
      // unrelated executables installed beside each other.
      if (!foundIndex && entry.isGame && !entry.path.empty()) {
        const std::wstring installPath = canonicalPathKey(entry.path);
        for (size_t index = 0; index < apps.size(); ++index) {
          const std::wstring targetPath = canonicalPathKey(apps[index].targetPath);
          if (!targetPath.empty() && pathIsInside(targetPath, installPath)) {
            foundIndex = index;
            break;
          }
        }
      }

      if (foundIndex) {
        auto& existing = apps[*foundIndex];
        if (entry.isGame && !existing.isGame) {
          const std::wstring visibleName = existing.name;
          const std::wstring localIcon = existing.iconKey;
          const std::wstring localTarget = existing.targetPath;
          const bool adminSupported = existing.adminSupported || entry.adminSupported;
          const bool systemEssential = existing.systemEssential || entry.systemEssential;
          entry.name = visibleName;
          if (!localIcon.empty()) entry.iconKey = localIcon;
          if (entry.targetPath.empty()) entry.targetPath = localTarget;
          entry.adminSupported = adminSupported;
          entry.systemEssential = systemEssential;
          entry.keywords = UniqueKeywords({
              feathercast::core::JoinKeywords(existing.keywords),
              feathercast::core::JoinKeywords(entry.keywords),
          });
          existing = std::move(entry);
          for (const auto& identity : candidateIdentities) {
            identities.emplace(identity, *foundIndex);
          }
          return;
        }
        existing.adminSupported = existing.adminSupported || entry.adminSupported;
        existing.systemEssential = existing.systemEssential || entry.systemEssential;
        existing.isGame = existing.isGame || entry.isGame;
        if (!entry.gameProvider.empty() &&
            existing.gameProvider.find(entry.gameProvider) == std::wstring::npos) {
          if (!existing.gameProvider.empty()) existing.gameProvider += L" + ";
          existing.gameProvider += entry.gameProvider;
        }
        if (entry.isGame && !entry.path.empty()) existing.path = entry.path;
        if (existing.iconKey.empty()) existing.iconKey = std::move(entry.iconKey);
        if (existing.targetPath.empty()) existing.targetPath = std::move(entry.targetPath);
        existing.keywords = UniqueKeywords({
            feathercast::core::JoinKeywords(existing.keywords),
            feathercast::core::JoinKeywords(entry.keywords),
        });
        for (const auto& identity : candidateIdentities) {
          identities.emplace(identity, *foundIndex);
        }
        return;
      }
      for (const auto& identity : candidateIdentities) {
        identities.emplace(identity, apps.size());
      }
      apps.push_back(std::move(entry));
    };

    for (const auto& path : StartMenuShortcutPaths(stopToken, generation)) {
      if (DiscoveryCanceled(stopToken, generation)) return std::nullopt;
      auto entry = ShortcutEntry(path);
      if (entry) addUnique(std::move(*entry));
    }

    for (auto& entry : AppsFolderEntries(stopToken, generation)) {
      if (DiscoveryCanceled(stopToken, generation)) return std::nullopt;
      addUnique(std::move(entry));
    }

    for (auto& entry : feathercast::games::DiscoverInstalledGames(apps, stopToken)) {
      if (DiscoveryCanceled(stopToken, generation)) return std::nullopt;
      addUnique(std::move(entry));
    }

    if (DiscoveryCanceled(stopToken, generation)) return std::nullopt;
    std::sort(apps.begin(), apps.end(), [](const AppEntry& a, const AppEntry& b) {
      return Lower(a.name) < Lower(b.name);
    });
    return apps;
  }

  std::vector<std::filesystem::path> StartMenuShortcutPaths(std::stop_token stopToken,
                                                            uint64_t generation) {
    std::vector<std::filesystem::path> dirs;
    wchar_t programData[MAX_PATH]{};
    wchar_t appData[MAX_PATH]{};
    GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (programData[0]) dirs.emplace_back(std::filesystem::path(programData) / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs");
    if (appData[0]) dirs.emplace_back(std::filesystem::path(appData) / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs");

    std::vector<std::filesystem::path> out;
    for (const auto& dir : dirs) {
      if (DiscoveryCanceled(stopToken, generation)) break;
      std::error_code ec;
      if (!std::filesystem::is_directory(dir, ec)) continue;
      size_t enumerated = 0;
      for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) continue;
        if (it->is_regular_file(ec) && Lower(it->path().extension().wstring()) == L".lnk") out.push_back(it->path());
        if ((++enumerated & 63u) == 0 && DiscoveryCanceled(stopToken, generation)) break;
      }
    }
    return out;
  }

  std::optional<AppEntry> ShortcutEntry(const std::filesystem::path& path) {
    const std::wstring name = CleanName(path.stem().wstring());
    if (name.empty() || ShouldSkipName(name)) return std::nullopt;

    ShortcutInfo shortcut;
    LoadShortcut(path.wstring(), shortcut);

    AppEntry entry;
    entry.id = path.wstring();
    entry.name = name;
    entry.path = path.wstring();
    entry.source = L"shortcut";
    entry.launchType = LaunchType::Shortcut;
    entry.launchTarget = path.wstring();
    entry.targetPath = shortcut.target;
    entry.args = shortcut.args;
    entry.cwd = shortcut.cwd;
    entry.iconKey = path.wstring();
    entry.adminSupported = true;
    entry.systemEssential = IsSystemEssentialName(name);
    entry.keywords = KeywordsFor(name, shortcut.target, L"");
    return entry;
  }

  std::vector<AppEntry> AppsFolderEntries(std::stop_token stopToken,
                                          uint64_t generation) {
    std::vector<AppEntry> out;
    PIDLIST_ABSOLUTE appsPidl = nullptr;
    if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &appsPidl, 0, nullptr))) return out;
    ScopeExit freeAppsPidl([&] { CoTaskMemFree(appsPidl); });

    ComPtr<IShellFolder> folder;
    if (SUCCEEDED(SHBindToObject(nullptr, appsPidl, nullptr, IID_PPV_ARGS(&folder)))) {
      ComPtr<IEnumIDList> enumList;
      if (SUCCEEDED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, enumList.GetAddressOf()))) {
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        constexpr int kNameBufferLen = 512;
        while (enumList->Next(1, &child, &fetched) == S_OK) {
          if (DiscoveryCanceled(stopToken, generation)) break;
          ScopeExit freeChild([&] { CoTaskMemFree(child); });
          STRRET str{};
          wchar_t nameBuf[kNameBufferLen]{};
          if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &str))) {
            StrRetToBufW(&str, child, nameBuf, kNameBufferLen);
          }

          ComPtr<IShellItem2> item;
          PWSTR appIdRaw = nullptr;
          if (SUCCEEDED(SHCreateItemWithParent(appsPidl, folder.Get(), child, IID_PPV_ARGS(&item)))) {
            item->GetString(PKEY_AppUserModel_ID, &appIdRaw);
          }
          CoMemPtr<wchar_t> appIdOwner(appIdRaw);

          const std::wstring name = CleanName(nameBuf);
          const std::wstring appId = appIdRaw ? appIdRaw : L"";

          if (!name.empty() && !appId.empty() && !ShouldSkipName(name)) {
            const bool terminal = Lower(name).find(L"terminal") != std::wstring::npos;
            const std::wstring terminalAlias = terminal ? FindWindowsAppAlias(L"wt.exe") : L"";

            AppEntry entry;
            entry.id = L"start:" + appId;
            entry.name = name;
            entry.appUserModelId = appId;
            entry.iconKey = L"appsFolder:" + appId;
            entry.systemEssential = IsSystemEssentialName(name);
            entry.keywords = KeywordsFor(name, terminalAlias, appId);

            entry.source = appId.find(L"!") != std::wstring::npos ? L"appx" : L"start";
            entry.launchType = LaunchType::AppsFolder;
            entry.launchTarget = appId;
            // Keep the execution alias (e.g. wt.exe) so the app can still be
            // launched elevated; normal launches go through AppsFolder
            // activation, which reliably brings the window to the foreground
            // instead of occasionally opening an Explorer folder.
            entry.targetPath = terminalAlias;
            entry.adminSupported = !entry.targetPath.empty();
            if (terminal) entry.systemEssential = true;
            out.push_back(std::move(entry));
          }
        }
      }
    }

    return out;
  }

  void PrecacheIcons(std::stop_token stopToken, uint64_t generation) {
    std::vector<std::wstring> keys;
    {
      std::lock_guard lock(dataMutex_);
      for (const auto& app : apps_) {
        if (!app.iconKey.empty()) keys.push_back(app.iconKey);
      }
    }
    caching_ = true;
    if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
    for (const auto& key : keys) {
      if (DiscoveryCanceled(stopToken, generation)) break;
      QueueIcon(key);
    }
    caching_ = false;
    if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
  }

  void RefreshWindows() {
    auto windows = ListWindows(hwnd_);
    {
      std::lock_guard lock(dataMutex_);
      windows_ = std::move(windows);
    }
    MarkSearchDataChanged();
  }

  void RefreshWindowsAsync() {
    if (windowRefreshPending_.exchange(true)) return;
    if (!launchExecutor_.Submit([this, own = hwnd_](std::stop_token stopToken) {
          auto windows = ListWindows(own);
          if (stopToken.stop_requested() || stopThreads_) {
            windowRefreshPending_.store(false);
            return;
          }
          {
            std::lock_guard lock(dataMutex_);
            windows_ = std::move(windows);
          }
          MarkSearchDataChanged();
          windowRefreshPending_.store(false);
          PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
        })) {
      windowRefreshPending_.store(false);
    }
  }

  std::vector<DisplayItem> BuiltInCommands() const {
    return feathercast::commands::BuildCommandItems();
  }

  std::vector<DisplayItem> ActionsFor(const DisplayItem& target) const {
    return feathercast::commands::BuildActions(target, settings_);
  }

  // Entry point for every "results changed" trigger (formerly BuildSections).
  // Cheap result sets (empty query, action mode) are computed synchronously so
  // callers that immediately show/position the window see correct sizing with
  // no flicker; the expensive full-pool search is offloaded to the worker.
  void MarkSearchDataChanged() {
    dataRevision_.fetch_add(1, std::memory_order_release);
  }

  // Persist settings and invalidate the cached search corpus, since several
  // settings (pinnedApps, appAliases, usageStats, hiddenApps, showStoreApps,
  // quicklinks, recentApps) feed into the snapshot's items and ranking.
  bool PersistSettings() {
    if (settingsPersistenceBlocked_) {
      SetSettingsStatus(
          StatusSeverity::Error,
          startupSettingsNotice_.empty()
              ? L"Automatic settings saves are disabled to protect settings.json."
              : startupSettingsNotice_);
      MarkSearchDataChanged();
      return false;
    }
    SetSettingsStatus(StatusSeverity::Progress, L"Saving settings...");
    if (!persistence_.SaveSettings(settings_)) {
      ReportPersistenceFailure(L"The persistence worker is unavailable.");
      MarkSearchDataChanged();
      return false;
    }
    MarkSearchDataChanged();
    return true;
  }

  bool SearchPending() const {
    return searchPresentation_.Pending();
  }

  bool ResultsActivationAllowed() const {
    return searchPresentation_.CanActivate();
  }

  void CancelResultPointerPress() {
    if (pointerPress_ && pointerPress_->type == HitType::Result) {
      CancelPointerPress(pointerPress_->owner);
    }
  }

  void RequestSearch() {
    overlayStatus_.reset();
    const auto requestedScope = feathercast::search_scope::Parse(query_).scope;
    if (settings_.fileIndexEnabled &&
        requestedScope == feathercast::search_scope::Scope::Files) {
      EnsureFileIndexLoaded();
      if (!fileIndexConfigured_) ConfigureFileIndex();
    }
    QueryRequest req = GatherRequest();
    if (std::abs(overlayVisualScroll_.Target() -
                 static_cast<double>(scroll_)) > 0.001) {
      RetargetOverlayScroll();
    }
    CancelResultPointerPress();
    if (visible_) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd_, OBJID_CLIENT, 1);
    if (!req.compactClear && !req.empty && !req.actionMode &&
        req.scope == feathercast::search_scope::Scope::All) {
      extensions_.RequestQuery(req.query, req.generation);
    }
    if (!req.compactClear && !req.actionMode &&
        req.browseView == BrowseView::None &&
        req.scope == feathercast::search_scope::Scope::Files) {
      if (!fileSearchService_.Query(
              {req.generation, req.query, req.limit,
               settings_.fileContentIndexEnabled})) {
        ReportBackgroundFailure(L"The file search worker is unavailable.");
      }
      return;
    }
    if (req.compactClear || req.empty || req.actionMode) {
      ApplyResults(ComputeResults(req));
    } else {
      DispatchToWorker(std::move(req));
    }
  }

  // UI thread: snapshot all inputs the engine needs. The heavy, query-independent
  // corpus is taken from a cached SearchSnapshot (rebuilt only when data/settings
  // change), so this stays cheap per keystroke. Only query/mode-specific fields
  // are gathered here. This is the only place settings_/apps_/windows_ are read
  // for a search, keeping ComputeResults pure.
  QueryRequest GatherRequest() {
    QueryRequest req;
    req.generation = searchPresentation_.NextRequest();
    req.query = query_;
    if (!actionMode_ && browseView_ == BrowseView::None) {
      const auto scoped = feathercast::search_scope::Parse(query_);
      if (scoped.recognized) {
        req.scope = scoped.scope;
        req.query = scoped.terms;
      }
    }

    const bool empty = Trim(req.query).empty();
    req.empty = empty;
    req.actionMode = actionMode_;
    req.browseView = browseView_;
    req.compactClear = settings_.compactMode && Trim(query_).empty() &&
                       view_ == View::Search &&
                       !actionMode_ && browseView_ == BrowseView::None;
    req.recentIds = std::set<std::wstring>(settings_.recentApps.begin(), settings_.recentApps.end());
    req.limit = std::clamp(settings_.maxResults, MIN_RESULTS, MAX_RESULT_SETTING);
    req.now = UnixNow();
    {
      const std::time_t captured = static_cast<std::time_t>(req.now);
      std::tm local{};
      if (localtime_s(&local, &captured) == 0) {
        req.clock.localDate =
            std::chrono::year{local.tm_year + 1900} /
            std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)} /
            std::chrono::day{static_cast<unsigned>(local.tm_mday)};
        req.clock.localHour = local.tm_hour;
        req.clock.localMinute = local.tm_min;
        req.clock.localSecond = local.tm_sec;
      }
      req.clock.epochSeconds = req.now;
    }
    req.searchEngines = settings_.searchEngines;
    {
      std::lock_guard lock(dataMutex_);
      req.currencyRates = currencyRates_.perUsd;
    }
    req.defaultCurrency = localeCurrency_;

    if (req.compactClear) return req;

    const uint64_t requestedRevision = dataRevision_.load(std::memory_order_acquire);
    if (!snapshot_) snapshot_ = std::make_shared<SearchSnapshot>();
    if (snapshotRevision_ != requestedRevision) ScheduleSnapshotBuild(requestedRevision);
    req.snapshot = snapshot_;

    // Action mode builds a small, target-specific list each call.
    if (req.actionMode) {
      req.actions = ActionsFor(actionTarget_);
      if (!empty) {
        for (const auto& item : req.actions) req.actionSearchItems.push_back(ToSearchItem(item));
      }
      return req;
    }

    // Query-dependent extension results (read from the extension result cache).
    if (!empty) {
      for (const auto& extensionResult : extensions_.CachedResultsFor(req.query)) {
        req.extensionItems.push_back(ExtensionDisplay(extensionResult));
      }
    }

    return req;
  }

  // Builds the query-independent search corpus once per data/settings version.
  // Expensive (deep-copies every app/window/file and derives a SearchItem each),
  // so it runs only when the data revision changes, not on every keystroke.
  std::shared_ptr<const SearchSnapshot> BuildSnapshot(
      const Settings& snapshotSettings) {
    auto snap = std::make_shared<SearchSnapshot>();

    std::vector<AppEntry> apps;
    std::vector<WindowEntry> windows;
    std::vector<AppEntry> files;
    std::vector<feathercast::snippets::Snippet> snippets;
    std::vector<ClipboardEntry> clipboard;
    {
      std::lock_guard lock(dataMutex_);
      apps = apps_;
      windows = windows_;
      files = fileIndex_;
      snippets = snippets_;
      clipboard = clipboardHistory_;
    }

    std::vector<DisplayItem> appItems;
    for (const auto& app : apps) {
      if (ContainsAnyAppKey(snapshotSettings.hiddenApps, app)) continue;
      if (!snapshotSettings.showStoreApps && IsStoreLikeSource(app.source)) continue;
      appItems.push_back(AppDisplay(app));
    }
    for (const auto& link : snapshotSettings.quicklinks) {
      if (link.keyword.empty() || link.target.empty()) continue;
      appItems.push_back(AppDisplay(QuicklinkApp(link)));
    }
    for (const auto& folder : systemFolders_) {
      if (ContainsAnyAppKey(snapshotSettings.hiddenApps, folder)) continue;
      appItems.push_back(AppDisplay(folder));
    }
    for (const auto& snippet : snippets) {
      snap->snippetItems.push_back(SnippetDisplay(snippet));
    }
    if (snapshotSettings.clipboardHistoryEnabled) {
      for (const auto& entry : clipboard) {
        snap->clipboardItems.push_back(ClipboardDisplay(entry));
      }
    }
    for (const auto& item : snap->clipboardItems) {
      snap->clipboardSearchItems.push_back(ToSearchItem(item, snapshotSettings));
    }

    for (const auto& item : appItems) {
      if (item.app.isGame) snap->gameItems.push_back(item);
    }
    std::sort(snap->gameItems.begin(), snap->gameItems.end(),
              [](const DisplayItem& left, const DisplayItem& right) {
                return Lower(left.app.name) < Lower(right.app.name);
              });
    for (const auto& item : snap->gameItems) {
      snap->gameSearchItems.push_back(ToSearchItem(item, snapshotSettings));
    }

    std::vector<DisplayItem> windowItems;
    if (snapshotSettings.showOpenWindows) {
      for (const auto& window : windows) windowItems.push_back(WindowDisplay(window));
    }
    auto commandItems = BuiltInCommands();

    // Empty-state buckets.
    {
      std::map<std::wstring, DisplayItem> byId;
      for (const auto& item : appItems) {
        for (const auto& key : AppKeys(item.app)) byId[key] = item;
      }
      for (const auto& item : appItems) {
        if (ContainsAnyAppKey(snapshotSettings.pinnedApps, item.app)) snap->pinned.push_back(item);
      }
      for (const auto& id : snapshotSettings.recentApps) {
        if (byId.contains(id)) snap->recent.push_back(byId[id]);
      }
      for (const auto& item : appItems) {
        if (!item.app.isGame &&
            (item.app.systemEssential || item.app.source == L"alias" ||
             item.app.source == L"quicklink")) {
          snap->system.push_back(item);
        }
        if (item.app.source == L"system-folder") snap->systemFolders.push_back(item);
      }
    }
    snap->windowItems = windowItems;    // also folded into the pool below
    snap->commandItems = commandItems;  // also folded into the pool below

    // Non-empty general search pool (parallel to searchItems). Files & folders
    // are only searched when there is a query, so they live solely in the pool.
    std::vector<DisplayItem> pool = std::move(windowItems);
    pool.insert(pool.end(), snap->snippetItems.begin(), snap->snippetItems.end());
    pool.insert(pool.end(), snap->clipboardItems.begin(), snap->clipboardItems.end());
    pool.insert(pool.end(), appItems.begin(), appItems.end());
    pool.insert(pool.end(), commandItems.begin(), commandItems.end());
    if (snapshotSettings.fileIndexEnabled) {
      for (const auto& file : files) pool.push_back(AppDisplay(file));
    }
    std::vector<feathercast::core::PreparedSearchItem> searchItems;
    searchItems.reserve(pool.size());
    for (const auto& item : pool) {
      searchItems.push_back(feathercast::core::PrepareSearchItem(ToSearchItem(item, snapshotSettings)));
    }
    snap->searchItems = std::move(searchItems);
    snap->pool = std::move(pool);

    return snap;
  }

  void ScheduleSnapshotBuild(uint64_t revision) {
    if (snapshotRevision_ == revision || snapshotScheduledRevision_ == revision) return;
    SnapshotBuildRequest request;
    request.revision = revision;
    request.settings = settings_;
    snapshotScheduledRevision_ = revision;
    if (!snapshotCoordinator_.UpdateCorpus(std::move(request))) {
      snapshotScheduledRevision_ = 0;
      ReportBackgroundFailure(L"The search snapshot worker is unavailable.");
    }
  }

  void OnSnapshotReady(SnapshotBuildResult result) {
    if (!result.snapshot) return;

    const uint64_t currentRevision = dataRevision_.load(std::memory_order_acquire);
    if (result.revision != currentRevision) {
      ScheduleSnapshotBuild(currentRevision);
      return;
    }
    snapshot_ = std::move(result.snapshot);
    snapshotRevision_ = result.revision;
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // Pure engine: QueryRequest -> ResultsCollection. No member/UI state access,
  // so it is safe to run on the search worker thread.
  static ResultsCollection ComputeResults(const QueryRequest& req) {
    return feathercast::search_pipeline::ComputeResults(req);
  }

  struct ResultPositions {
    std::map<std::wstring, float> rows;
    std::map<std::wstring, float> headers;
  };

  ResultPositions CaptureResultPositions() const {
    ResultPositions positions;
    float y = kResultsTop;
    for (const auto& section : sections_) {
      float headerY = y;
      if (const auto motion = resultHeaderMotion_.find(section.title);
          motion != resultHeaderMotion_.end()) {
        headerY += static_cast<float>(motion->second.offsetY.Value());
      }
      positions.headers[section.title] = headerY;
      y += kSectionHeaderHeight;
      for (const auto& item : section.items) {
        const std::wstring key = item.Key();
        float rowY = y;
        if (const auto motion = resultRowMotion_.find(key);
            motion != resultRowMotion_.end()) {
          rowY += static_cast<float>(motion->second.offsetY.Value());
        }
        positions.rows[key] = rowY;
        y += kResultRowStride;
      }
    }
    return positions;
  }

  void StartResultTransitions(const ResultPositions& oldPositions) {
    resultRowMotion_.clear();
    resultHeaderMotion_.clear();
    if (!FadeAnimationsAllowed() || !visible_) return;

    float y = kResultsTop;
    for (const auto& section : sections_) {
      auto& header = resultHeaderMotion_[section.title];
      const auto oldHeader = oldPositions.headers.find(section.title);
      header.offsetY.Snap(
          SpatialAnimationsAllowed()
              ? (oldHeader == oldPositions.headers.end()
                     ? 4.0
                     : oldHeader->second - y)
              : 0.0);
      header.opacity.Snap(oldHeader == oldPositions.headers.end() ? 0.65
                                                                  : 1.0);
      header.offsetY.Retarget(0.0, kResultTransitionSeconds,
                              SpatialAnimationsAllowed());
      header.opacity.Retarget(1.0, kResultTransitionSeconds, true);
      y += kSectionHeaderHeight;
      for (const auto& item : section.items) {
        const std::wstring key = item.Key();
        auto& row = resultRowMotion_[key];
        const auto oldRow = oldPositions.rows.find(key);
        row.offsetY.Snap(
            SpatialAnimationsAllowed()
                ? (oldRow == oldPositions.rows.end() ? 4.0
                                                     : oldRow->second - y)
                : 0.0);
        row.opacity.Snap(oldRow == oldPositions.rows.end() ? 0.65 : 1.0);
        row.offsetY.Retarget(0.0, kResultTransitionSeconds,
                             SpatialAnimationsAllowed());
        row.opacity.Retarget(1.0, kResultTransitionSeconds, true);
        y += kResultRowStride;
      }
    }
    RequestAnimationFrame();
  }

  // UI thread: commit a freshly computed result set to the rendered state.
  void ApplyResults(ResultsCollection result) {
    if (!searchPresentation_.Publish(result.generation)) return;
    const ResultPositions oldPositions = CaptureResultPositions();
    const bool preserveSelection = query_ == displayedQuery_;
    const std::wstring selectedKey =
        preserveSelection && selected_ >= 0 &&
                selected_ < static_cast<int>(flatItems_.size())
            ? flatItems_[static_cast<size_t>(selected_)].Key()
            : std::wstring{};
    sections_ = std::move(result.sections);
    flatItems_ = std::move(result.flatItems);
    if (pendingNavigationRestore_) {
      const auto& restore = *pendingNavigationRestore_;
      const auto match = std::find_if(
          flatItems_.begin(), flatItems_.end(),
          [&](const DisplayItem& item) { return item.Key() == restore.selectedKey; });
      feathercast::ui::OverlayController::RestoreResultPosition(
          overlayState_,
          match != flatItems_.end()
              ? static_cast<int>(std::distance(flatItems_.begin(), match))
              : restore.selected,
          restore.scroll);
      pendingNavigationRestore_.reset();
    } else if (!selectedKey.empty()) {
      const auto match = std::find_if(
          flatItems_.begin(), flatItems_.end(), [&](const DisplayItem& item) {
            return item.Key() == selectedKey;
          });
      if (match != flatItems_.end()) {
        feathercast::ui::OverlayController::Select(
            overlayState_,
            static_cast<int>(std::distance(flatItems_.begin(), match)),
            static_cast<int>(flatItems_.size()), false);
      }
    }
    displayedQuery_ = query_;
    if (selected_ >= static_cast<int>(flatItems_.size())) selected_ = std::max<int>(0, static_cast<int>(flatItems_.size()) - 1);
    if (!pendingNavigation_.Empty()) {
      feathercast::ui::OverlayController::Select(
          overlayState_,
          pendingNavigation_.Apply(selected_,
                                   static_cast<int>(flatItems_.size())),
          static_cast<int>(flatItems_.size()), false);
      pendingNavigation_.Clear();
      EnsureSelectedVisible();
    }
    ApplyWindowSize();
    StartResultTransitions(oldPositions);
    SyncSelectionAnimationToTarget();
    ScheduleSelectedPreview();
    NotifyWinEvent(EVENT_OBJECT_REORDER, hwnd_, OBJID_CLIENT, CHILDID_SELF);
  }

  // Hand the newest request to the worker, coalescing any unstarted request.
  void DispatchToWorker(QueryRequest req) {
    if (!searchCoordinator_.Query(std::move(req))) {
      ReportBackgroundFailure(L"The search worker is unavailable.");
    }
  }

  feathercast::core::SearchItem ToSearchItem(const DisplayItem& item,
                                             const Settings& searchSettings) const {
    feathercast::core::SearchItem out;
    if (item.isCalculator) {
      out.id = item.Key();
      out.kind = L"calculator";
      out.source = L"calculator";
      out.name = item.calculationResult;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.isExtension) {
      out.id = item.Key();
      out.kind = L"extension";
      out.source = L"extension";
      out.name = item.extension.title;
      out.keywords = item.extension.keywords;
      out.keywords.push_back(item.extension.pluginName);
      out.targetPath = item.extension.iconPath;
      out.systemEssential = true;
    } else if (item.isSnippet) {
      out.id = item.Key();
      out.kind = L"snippet";
      out.source = L"snippet";
      out.name = item.snippet.name;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.isClipboard) {
      out.id = item.Key();
      out.kind = L"clipboard";
      out.source = L"clipboard";
      out.name = item.clipboard.preview;
      out.keywords = item.commandKeywords;
      out.lastUsed = item.clipboard.capturedAt;
    } else if (item.isRunCommand) {
      out.id = item.Key();
      out.kind = L"run";
      out.source = L"run";
      out.name = item.runCommand.label;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.isSymbol) {
      out.id = item.Key();
      out.kind = L"symbol";
      out.source = L"symbol";
      out.name = item.symbol.label;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.utility) {
      out.id = item.Key();
      out.kind = L"utility";
      out.source = L"utility";
      out.name = item.utility->title;
      out.keywords = item.utility->keywords;
      out.keywords.push_back(item.utility->value);
      out.systemEssential = true;
    } else if (item.isCommand) {
      out.id = item.Key();
      out.kind = L"command";
      out.source = L"command";
      out.name = item.commandName;
      out.keywords = item.commandKeywords;
      out.systemEssential = true;
    } else if (item.isAction) {
      out.id = item.Key();
      out.kind = L"action";
      out.source = L"action";
      out.name = item.commandName;
      out.keywords = item.commandKeywords;
      out.keywords.push_back(item.commandDetail);
      out.systemEssential = true;
    } else if (item.isWindow) {
      out.id = L"win:" + std::to_wstring(reinterpret_cast<uintptr_t>(item.window.hwnd));
      out.kind = L"window";
      out.name = item.window.name;
      out.processName = item.window.processName;
      out.exe = item.window.exe;
    } else {
      out.id = item.app.id;
      out.path = item.app.path;
      out.kind = L"app";
      out.source = item.app.source;
      out.name = item.app.name;
      out.targetPath = item.app.targetPath;
      out.launchTarget = item.app.launchTarget;
      out.keywords = item.app.keywords;
      out.systemEssential = item.app.systemEssential;
      out.pinned = ContainsAnyAppKey(searchSettings.pinnedApps, item.app);
      for (const auto& key : AppKeys(item.app)) {
        if (auto alias = searchSettings.appAliases.find(key); alias != searchSettings.appAliases.end()) {
          out.keywords.push_back(alias->second);
        }
        if (auto usage = searchSettings.usageStats.find(key); usage != searchSettings.usageStats.end()) {
          out.usageCount = std::max(out.usageCount, usage->second.launches);
          out.lastUsed = std::max(out.lastUsed, usage->second.lastUsed);
        }
      }
    }
    return out;
  }

  feathercast::core::SearchItem ToSearchItem(const DisplayItem& item) const {
    return ToSearchItem(item, settings_);
  }

  HMONITOR ResolveOverlayMonitor(HWND foreground) const {
    POINT cursor{};
    if (GetCursorPos(&cursor)) return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (foreground && IsWindow(foreground)) return MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if (overlayRestoreCandidate_ &&
        ValidateRestoreCandidate(*overlayRestoreCandidate_)) {
      return MonitorFromWindow(overlayRestoreCandidate_->hwnd,
                               MONITOR_DEFAULTTONEAREST);
    }
    return MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  }

  MONITORINFO OverlayMonitorInfo() const {
    HMONITOR monitor = overlayMonitor_;
    if (!monitor) monitor = ResolveOverlayMonitor(GetForegroundWindow());
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) {
      POINT cursor{};
      GetCursorPos(&cursor);
      monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
      GetMonitorInfoW(monitor, &mi);
    }
    return mi;
  }

  void UpdateBackgroundState() {
    const bool isForeground =
        visible_ || volumeVisible_ ||
        (settingsHwnd_ && IsWindowVisible(settingsHwnd_));
    if (isForeground) {
      EnterInteractiveScheduling();
    } else {
      // File watchers and the indexing worker are only needed while the
      // launcher is actively being used. Stop them with the overlay so a
      // hidden launcher does not keep crawling user folders.
      StopFileIndexService();
      // Drop query-independent snapshots and decoded icon buffers while idle.
      // They are rebuilt lazily on the next invocation and can otherwise keep
      // hundreds of megabytes resident after a large discovery/index pass.
      snapshot_.reset();
      snapshotRevision_ = 0;
      snapshotScheduledRevision_ = 0;
      ClearIconBitmaps();
      iconResolver_.ClearPending();
      StopIconThreads();

      {
        std::lock_guard lock(dataMutex_);
        fileIndex_.clear();
      }
      fileSearchService_.UpdateFiles({});
      fileIndexLoaded_ = false;

      // Only query-specific results are discarded here; the source data stays
      // available for the next search without another discovery pass.
      searchPresentation_.Invalidate();
      searchCoordinator_.Invalidate(searchPresentation_.Requested());
      fileSearchService_.Invalidate(searchPresentation_.Requested());
      sections_.clear();
      flatItems_.clear();
      hits_.clear();

      // Background work remains unobtrusive without forcing page faults,
      // re-decoding icons, or restarting plugin hosts on every reopen.
      EnterBackgroundScheduling();
      SetTimer(hwnd_, TIMER_MEM_TRIM, 1000, nullptr);
    }
  }

  void EnterInteractiveScheduling() {
    KillTimer(hwnd_, TIMER_MEM_TRIM);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    MEMORY_PRIORITY_INFORMATION memPriority{};
    memPriority.MemoryPriority = MEMORY_PRIORITY_NORMAL;
    SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority,
                          &memPriority, sizeof(memPriority));

    PROCESS_POWER_THROTTLING_STATE powerThrottling{};
    powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    powerThrottling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                          &powerThrottling, sizeof(powerThrottling));
  }

  void EnterBackgroundScheduling() {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    MEMORY_PRIORITY_INFORMATION memPriority{};
    memPriority.MemoryPriority = MEMORY_PRIORITY_LOW;
    SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority,
                          &memPriority, sizeof(memPriority));

    // Let Windows infer background QoS from the hidden window and reduced
    // priority. Explicit EcoQoS can keep the first interactive frame on a
    // power-efficient core long enough for a reveal to miss its cadence.
    PROCESS_POWER_THROTTLING_STATE powerThrottling{};
    powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    powerThrottling.ControlMask = 0;
    powerThrottling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                          &powerThrottling, sizeof(powerThrottling));
  }

  void ExecuteOverlayEffects(feathercast::ui::UiEffects effects) {
    using feathercast::ui::HasEffect;
    using feathercast::ui::UiEffect;
    if (HasEffect(effects, UiEffect::RequestSearch)) RequestSearch();
    if (HasEffect(effects, UiEffect::EnsureSelectionVisible)) {
      EnsureSelectedVisible();
    }
    if (HasEffect(effects, UiEffect::RefreshData)) StartAppDiscovery();
    if (HasEffect(effects, UiEffect::FocusWindow) && hwnd_) SetFocus(hwnd_);
    if (HasEffect(effects, UiEffect::CloseView)) {
      HideOverlay(OverlayCloseReason::ExplicitDismiss);
    }
    if (HasEffect(effects, UiEffect::Invalidate) && hwnd_) {
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

  void ExecuteSettingsEffects(feathercast::ui::UiEffects effects) {
    using feathercast::ui::HasEffect;
    using feathercast::ui::UiEffect;
    if (HasEffect(effects, UiEffect::PersistSettings)) PersistSettings();
    if (HasEffect(effects, UiEffect::RefreshData)) StartAppDiscovery();
    if (HasEffect(effects, UiEffect::FocusWindow) && settingsHwnd_) {
      SetFocus(settingsHwnd_);
    }
    if (HasEffect(effects, UiEffect::CloseView) && settingsHwnd_) {
      ShowWindow(settingsHwnd_, SW_HIDE);
    }
    if (HasEffect(effects, UiEffect::Invalidate) && settingsHwnd_) {
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    }
  }

  void AdoptRestoreCandidate(const ForegroundSnapshot& snapshot) {
    RestoreCandidate candidate;
    static_cast<ForegroundSnapshot&>(candidate) = snapshot;
    candidate.generation = overlayFocusSession_.Generation();
    if (!ValidateRestoreCandidate(candidate)) return;
    overlayRestoreCandidate_ = candidate;
    DebugFocusLog(
        L"candidate generation=" + std::to_wstring(candidate.generation) +
        L" hwnd=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(
                         candidate.hwnd)) +
        L" pid=" + std::to_wstring(candidate.processId) +
        L" tid=" + std::to_wstring(candidate.threadId));
  }

  void ObserveExternalForeground(HWND window) {
    const ULONGLONG now = GetTickCount64();
    if (!overlayFocusSession_.AcceptsExternalCandidate(now)) return;
    if (auto snapshot = CaptureForegroundSnapshot(window)) {
      AdoptRestoreCandidate(*snapshot);
    }
  }

  bool TryActivateOverlay() {
    if (!visible_ || !hwnd_ || !IsWindowVisible(hwnd_)) return false;

    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    BringWindowToTop(hwnd_);
    const BOOL requested = SetForegroundWindow(hwnd_);
    SetActiveWindow(hwnd_);
    SetFocus(hwnd_);
    bool activated = GetForegroundWindow() == hwnd_;

    if (!activated) {
      HWND foreground = GetForegroundWindow();
      const DWORD currentThreadId = GetCurrentThreadId();
      const DWORD foregroundThreadId =
          foreground && foreground != hwnd_
              ? GetWindowThreadProcessId(foreground, nullptr)
              : 0;
      const bool attached =
          foregroundThreadId != 0 && foregroundThreadId != currentThreadId &&
          AttachThreadInput(foregroundThreadId, currentThreadId, TRUE) != FALSE;
      if (attached) {
        BringWindowToTop(hwnd_);
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        SetFocus(hwnd_);
        activated = GetForegroundWindow() == hwnd_;
        AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
      }
    }

    DebugFocusLog(
        L"activate-overlay generation=" +
        std::to_wstring(overlayFocusSession_.Generation()) + L" requested=" +
        std::to_wstring(requested != FALSE) + L" verified=" +
        std::to_wstring(activated));
    return activated;
  }

  void RetryOverlayActivation() {
    if (!visible_) {
      KillTimer(hwnd_, TIMER_OVERLAY_ACTIVATE);
      overlayFocusSession_.End();
      return;
    }
    if (overlayClosing_) {
      KillTimer(hwnd_, TIMER_OVERLAY_ACTIVATE);
      return;
    }

    const ULONGLONG now = GetTickCount64();
    const bool reactivationRequested = overlayReactivationQueued_;
    overlayReactivationQueued_ = false;
    if (!overlayFocusSession_.GuardActive(now) && !reactivationRequested) {
      KillTimer(hwnd_, TIMER_OVERLAY_ACTIVATE);
      overlayFocusSession_.Expire(now);
      HWND foreground = GetForegroundWindow();
      if (foreground != hwnd_) {
        HideOverlay(OverlayCloseReason::PassiveFocusLoss);
      }
      return;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground == hwnd_) {
      if (overlayFocusSession_.Phase() ==
          feathercast::interaction::OverlayFocusPhase::Acquiring) {
        overlayFocusSession_.RecordActivationAttempt(true);
      }
      return;
    }

    ObserveExternalForeground(foreground);
    overlayFocusSession_.RecordActivationAttempt(TryActivateOverlay());
  }

  void BeginOverlayActivation() {
    KillTimer(hwnd_, TIMER_OVERLAY_ACTIVATE);
    if (!SetTimer(hwnd_, TIMER_OVERLAY_ACTIVATE,
                  OVERLAY_ACTIVATION_INTERVAL_MS, nullptr)) {
      const bool activated = TryActivateOverlay();
      overlayFocusSession_.RecordActivationAttempt(activated);
      overlayFocusSession_.End();
      if (!activated) HideOverlay(OverlayCloseReason::PassiveFocusLoss);
      return;
    }
    RetryOverlayActivation();
  }

  void HandleOverlayFocusLoss(HWND nextWindow) {
    if (!visible_ || suppressHide_ || overlayClosing_ ||
        overlayFocusSession_.Phase() ==
            feathercast::interaction::OverlayFocusPhase::Closing) {
      return;
    }
    const ULONGLONG now = GetTickCount64();
    if (overlayFocusSession_.GuardActive(now)) {
      if (nextWindow) ObserveExternalForeground(nextWindow);
      if (HWND foreground = GetForegroundWindow(); foreground != hwnd_) {
        ObserveExternalForeground(foreground);
      }
      overlayReactivationQueued_ = true;
      PostMessageW(hwnd_, WM_OVERLAY_RETRY_ACTIVATION, 0, 0);
      return;
    }
    HideOverlay(OverlayCloseReason::PassiveFocusLoss);
  }

  void ShowOverlay(
      View view,
      std::optional<ForegroundSnapshot> foregroundSnapshot = std::nullopt) {
    // Restore foreground scheduling before search preparation, surface
    // resizing, or the first hidden-frame render can consume the reveal budget.
    EnterInteractiveScheduling();
    if (!foregroundSnapshot) {
      foregroundSnapshot = CaptureForegroundSnapshot(GetForegroundWindow());
    }
    const bool wasVisible = visible_;
    const auto previousCandidate = overlayRestoreCandidate_;
    overlayClosing_ = false;
    overlayReactivationQueued_ = false;
    pendingOverlayClose_.reset();
    CancelPointerPress();
    if (volumeVisible_) HideVolumeControl(false);
    if (settingsHwnd_ && IsWindowVisible(settingsHwnd_)) {
      HideSettings(false);
    }
    const auto effects =
        feathercast::ui::OverlayController::ResetForShow(overlayState_, view);
    const std::uint64_t generation =
        overlayFocusSession_.Begin(GetTickCount64());
    overlayRestoreCandidate_.reset();
    if (foregroundSnapshot) {
      AdoptRestoreCandidate(*foregroundSnapshot);
    } else if (wasVisible && previousCandidate &&
               ValidateRestoreCandidate(*previousCandidate)) {
      RestoreCandidate retained = *previousCandidate;
      retained.generation = generation;
      overlayRestoreCandidate_ = retained;
    }
    HWND foreground = foregroundSnapshot ? foregroundSnapshot->hwnd
                                         : GetForegroundWindow();
    overlayMonitor_ = ResolveOverlayMonitor(foreground);
    visible_ = true;
    UpdateBackgroundState();
    SyncSelectionAnimationToTarget();
    ExecuteOverlayEffects(effects);
    PositionWindow();
    // Reassert the transparent window treatment each reveal.
    ApplyGlass(hwnd_);
    const bool surfaceReady = overlaySurface_.dc != nullptr ||
                              PrewarmGlassSurface(hwnd_, overlaySurface_);
    animating_ = FadeAnimationsAllowed() && surfaceReady;
    // The actual reveal clock begins only after the hidden initial frame has
    // been rendered and foreground activation has completed.
    overlayRevealTimeline_.Reset();
    if (!wasVisible) {
      if (surfaceReady) {
        StartSurfaceOpen(hwnd_, overlayBounds_, overlayOpacity_, true);
      } else {
        RECT windowBounds{};
        GetWindowRect(hwnd_, &windowBounds);
        SnapWindowBounds(overlayBounds_, windowBounds);
        overlayOpacity_.Snap(1.0);
      }
    } else {
      overlayOpacity_.Retarget(1.0, kSurfaceOpenSeconds,
                               animating_);
    }

    // Prepare the reveal's zero-progress frame while the HWND is still hidden.
    // Otherwise DWM can briefly reveal the swap chain's previous full frame.
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);

    // Show the HWND only once. PositionWindow already established its topmost
    // z-order, so a second SWP_SHOWWINDOW transition only causes visible churn.
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RefreshWindowsAsync();

    DebugFocusLog(L"show generation=" + std::to_wstring(generation));
    BeginOverlayActivation();

    // Ignore the mouse pointer until it is actually moved, so an overlay that
    // opens under the cursor does not hover-select the result beneath it.
    GetCursorPos(&mouseAnchor_);
    ignoreMouseUntilMove_ = true;

    SetTimer(hwnd_, 1, 200, nullptr);
    if (animating_) {
      overlayRevealTimeline_.Start(AnimFinishMs() / 1000.0);
    }
    RestartAnimationClock();
  }

  bool ActivateRestoreCandidate(const RestoreCandidate& candidate) {
    if (!ValidateRestoreCandidate(candidate)) return false;
    if (IsIconic(candidate.hwnd)) ShowWindowAsync(candidate.hwnd, SW_RESTORE);
    BOOL requested = SetForegroundWindow(candidate.hwnd);
    bool activated = GetForegroundWindow() == candidate.hwnd;
    if (!activated) {
      const DWORD currentThreadId = GetCurrentThreadId();
      const bool attached =
          candidate.threadId != currentThreadId &&
          AttachThreadInput(candidate.threadId, currentThreadId, TRUE) != FALSE;
      if (attached) {
        BringWindowToTop(candidate.hwnd);
        requested = SetForegroundWindow(candidate.hwnd);
        activated = GetForegroundWindow() == candidate.hwnd;
        AttachThreadInput(candidate.threadId, currentThreadId, FALSE);
      }
    }
    DebugFocusLog(
        L"restore generation=" + std::to_wstring(candidate.generation) +
        L" requested=" + std::to_wstring(requested != FALSE) +
        L" verified=" + std::to_wstring(activated));
    return activated;
  }

  void FinishHideOverlay(PendingOverlayClose close) {
    if (!overlayFocusSession_.CanFinishClose(close.generation)) return;

    const bool explicitRestore =
        close.reason == OverlayCloseReason::ExplicitDismiss;
    HWND foreground = GetForegroundWindow();
    const bool candidateValid =
        close.candidate && close.candidate->generation == close.generation &&
        ValidateRestoreCandidate(*close.candidate);
    const bool overlayStillForeground = !foreground || foreground == hwnd_;
    if (feathercast::interaction::ShouldAttemptOverlayRestore(
            explicitRestore, overlayStillForeground, candidateValid)) {
      ActivateRestoreCandidate(*close.candidate);
    } else if (explicitRestore && foreground && foreground != hwnd_) {
      DebugFocusLog(
          L"restore-skipped-external generation=" +
          std::to_wstring(close.generation));
    }

    // Every hide ends the focus-restore session, including launch, paste and
    // focus-loss paths that intentionally do not restore the previous window.
    overlayRestoreCandidate_.reset();
    pendingOverlayClose_.reset();
    overlayClosing_ = false;
    CancelPointerPress(hwnd_);
    KillTimer(hwnd_, 1);
    KillTimer(hwnd_, TIMER_OVERLAY_ACTIVATE);
    overlayReactivationQueued_ = false;
    overlayFocusSession_.End();
    animating_ = false;
    overlayRevealTimeline_.Reset();
    animatingSelection_ = false;
    animationFrameQueued_ = false;
    visualSelectedY_ = -1.0f;
    StopSelectionAnimationTimer();
    KillTimer(hwnd_, TIMER_PREVIEW_LOAD);
    previewOpen_ = false;
    ++previewGeneration_;
    previewService_.Invalidate(previewGeneration_);
    previewResult_.reset();
    previewBitmap_.Reset();
    previewScroll_ = 0.0f;
    confirmation_.reset();
    confirmationFocus_ = 0;
    confirmationHover_ = -1;
    confirmationClosing_ = false;
    confirmationProgress_.Snap(0.0);
    pendingNavigation_.Clear();
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    overlayMonitor_ = nullptr;
    UpdateBackgroundState();
  }

  void HideOverlay(OverlayCloseReason reason) {
    if (!visible_) return;
    if (overlayClosing_) return;

    KillTimer(hwnd_, TIMER_OVERLAY_ACTIVATE);
    const bool restoreFocus = reason == OverlayCloseReason::ExplicitDismiss;
    PendingOverlayClose close;
    close.generation = overlayFocusSession_.Generation();
    close.reason = reason;
    if (restoreFocus && overlayRestoreCandidate_ &&
        overlayRestoreCandidate_->generation == close.generation) {
      close.candidate = overlayRestoreCandidate_;
    }
    overlayFocusSession_.BeginClosing();
    DebugFocusLog(L"close generation=" + std::to_wstring(close.generation) +
                  L" reason=" +
                  std::to_wstring(static_cast<int>(reason)));

    if (restoreFocus && FadeAnimationsAllowed()) {
      pendingOverlayClose_ = close;
      overlayClosing_ = true;
      CancelPointerPress(hwnd_);
      KillTimer(hwnd_, 1);
      overlayOpacity_.Retarget(0.0, kSurfaceCloseSeconds, true);
      RequestAnimationFrame();
      return;
    }
    FinishHideOverlay(std::move(close));
  }

  static RectF VolumeTrackRect(float width) {
    return {28.0f, 82.0f, width - 28.0f, 94.0f};
  }

  static RectF VolumeTrackHitRect(float width) {
    const RectF track = VolumeTrackRect(width);
    return {track.left - 4.0f, track.top - 16.0f, track.right + 4.0f,
            track.bottom + 16.0f};
  }

  void NotifyVolumeValueChanged() {
    if (!volumeHwnd_) return;
    InvalidateRect(volumeHwnd_, nullptr, FALSE);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, volumeHwnd_, OBJID_CLIENT, 1);
  }

  void RetargetVolumeVisual(bool animate) {
    volumeVisualPercent_.Retarget(
        static_cast<double>(volumePercent_), 0.080,
        animate && SpatialAnimationsAllowed());
    if (volumeVisualPercent_.Active()) RequestAnimationFrame();
  }

  void SetVolumeStatus(std::wstring status) {
    if (volumeStatus_ == status) return;
    volumeStatus_ = std::move(status);
    if (volumeHwnd_) InvalidateRect(volumeHwnd_, nullptr, FALSE);
  }

  void RefreshVolumeFromSystem() {
    if (!volumeVisible_ || volumeDragging_) return;
    const auto current = feathercast::audio::ReadDefaultOutputVolumePercent();
    if (!current) {
      SetVolumeStatus(L"Could not read the default output volume.");
      return;
    }
    SetVolumeStatus({});
    if (*current == volumePercent_) return;
    volumePercent_ = *current;
    RetargetVolumeVisual(true);
    NotifyVolumeValueChanged();
  }

  void ApplyVolumePercent(int percent) {
    const int next = feathercast::audio::ClampPercent(percent);
    if (next == volumePercent_) return;
    if (!feathercast::audio::SetDefaultOutputVolumePercent(next)) {
      SetVolumeStatus(L"Could not change the default output volume.");
      return;
    }
    volumePercent_ = next;
    RetargetVolumeVisual(!volumeDragging_);
    SetVolumeStatus({});
    NotifyVolumeValueChanged();
  }

  void SetVolumeFromTrack(float x, float width) {
    const RectF track = VolumeTrackRect(width);
    ApplyVolumePercent(
        feathercast::audio::PercentFromTrack(x, track.left, track.right));
  }

  void HandleVolumeKeyDown(UINT vk) {
    switch (vk) {
      case VK_ESCAPE:
        HideVolumeControl(true);
        return;
      case VK_LEFT:
      case VK_DOWN:
        ApplyVolumePercent(
            feathercast::audio::AdjustPercent(volumePercent_, -1));
        return;
      case VK_RIGHT:
      case VK_UP:
        ApplyVolumePercent(
            feathercast::audio::AdjustPercent(volumePercent_, 1));
        return;
      case VK_NEXT:
        ApplyVolumePercent(
            feathercast::audio::AdjustPercent(volumePercent_, -10));
        return;
      case VK_PRIOR:
        ApplyVolumePercent(
            feathercast::audio::AdjustPercent(volumePercent_, 10));
        return;
      case VK_HOME:
        ApplyVolumePercent(0);
        return;
      case VK_END:
        ApplyVolumePercent(100);
        return;
      default:
        return;
    }
  }

  void PositionVolumeWindow(HMONITOR monitor) {
    if (!monitor) monitor = ResolveOverlayMonitor(GetForegroundWindow());
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return;
    const float scale = GetMonitorScale(monitor);
    const int width = static_cast<int>(VOLUME_WIDTH * scale);
    const int height = static_cast<int>(VOLUME_HEIGHT * scale);
    const int x = info.rcWork.left +
                  ((info.rcWork.right - info.rcWork.left) - width) / 2;
    const int y = info.rcWork.top + static_cast<int>(
                                      (info.rcWork.bottom - info.rcWork.top) *
                                      0.22f);
    SetWindowPos(volumeHwnd_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE);
  }

  void ShowVolumeControl() {
    const auto current = feathercast::audio::ReadDefaultOutputVolumePercent();
    if (!current) {
      SetOverlayStatus(StatusSeverity::Error,
                       L"Could not read the default output volume.");
      return;
    }

    HMONITOR monitor = overlayMonitor_;
    if (!monitor) monitor = ResolveOverlayMonitor(GetForegroundWindow());
    volumeRestoreWindow_ =
        overlayRestoreCandidate_ ? overlayRestoreCandidate_->hwnd : nullptr;
    HideOverlay(OverlayCloseReason::Transition);
    EnterInteractiveScheduling();

    volumePercent_ = *current;
    volumeStatus_.clear();
    volumeMonitor_ = monitor;
    volumeVisible_ = true;
    volumeClosing_ = false;
    pendingVolumeRestore_ = nullptr;
    volumeVisualPercent_.Snap(static_cast<double>(volumePercent_));
    PositionVolumeWindow(monitor);
    ApplyGlass(volumeHwnd_);
    const bool surfaceReady =
        PrewarmGlassSurface(volumeHwnd_, volumeSurface_);
    if (surfaceReady) {
      StartSurfaceOpen(volumeHwnd_, volumeBounds_, volumeOpacity_, false);
    } else {
      RECT windowBounds{};
      GetWindowRect(volumeHwnd_, &windowBounds);
      SnapWindowBounds(volumeBounds_, windowBounds);
      volumeOpacity_.Snap(1.0);
    }
    RedrawWindow(volumeHwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    ShowWindow(volumeHwnd_, SW_SHOWNORMAL);
    SetWindowPos(volumeHwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(volumeHwnd_);
    SetActiveWindow(volumeHwnd_);
    SetFocus(volumeHwnd_);
    SetTimer(volumeHwnd_, TIMER_VOLUME_REFRESH, 250, nullptr);
    UpdateBackgroundState();
    NotifyWinEvent(EVENT_OBJECT_FOCUS, volumeHwnd_, OBJID_CLIENT, 1);
    RestartAnimationClock();
  }

  void FinishHideVolumeControl(HWND restoreTarget) {
    volumeClosing_ = false;
    pendingVolumeRestore_ = nullptr;
    volumeRestoreWindow_ = nullptr;
    volumeVisible_ = false;
    volumeDragging_ = false;
    KillTimer(volumeHwnd_, TIMER_VOLUME_REFRESH);
    if (GetCapture() == volumeHwnd_) ReleaseCapture();
    ShowWindow(volumeHwnd_, SW_HIDE);
    volumeMonitor_ = nullptr;
    volumeStatus_.clear();
    UpdateBackgroundState();
    if (restoreTarget) FocusWindow(restoreTarget);
  }

  void HideVolumeControl(bool restoreFocus) {
    if (!volumeVisible_) return;
    HWND restoreTarget = restoreFocus ? volumeRestoreWindow_ : nullptr;
    if (restoreFocus && FadeAnimationsAllowed() && !volumeClosing_) {
      pendingVolumeRestore_ = restoreTarget;
      volumeClosing_ = true;
      volumeOpacity_.Retarget(0.0, kSurfaceCloseSeconds, true);
      RequestAnimationFrame();
      return;
    }
    FinishHideVolumeControl(restoreTarget);
  }

  void ToggleOverlay(
      std::optional<ForegroundSnapshot> foregroundSnapshot = std::nullopt) {
    if (volumeVisible_) HideVolumeControl(true);
    else if (visible_ && overlayClosing_) {
      ShowOverlay(View::Search, std::move(foregroundSnapshot));
    } else if (visible_) {
      HideOverlay(OverlayCloseReason::ExplicitDismiss);
    } else {
      ShowOverlay(View::Search, std::move(foregroundSnapshot));
    }
  }

  void TriggerShortcutToggle(
      std::optional<ForegroundSnapshot> foregroundSnapshot) {
    const ULONGLONG now = GetTickCount64();
    if (now - lastShortcutToggleTick_ < 250) return;
    lastShortcutToggleTick_ = now;
    ToggleOverlay(std::move(foregroundSnapshot));
  }

  void OpenSettings() {
    CancelPointerPress();
    if (volumeVisible_) HideVolumeControl(false);
    HideOverlay(OverlayCloseReason::Transition);
    // A successful save is transient feedback for the settings session that
    // caused it. Do not carry it into a later session, where it would imply
    // that the user had already changed something after opening Settings.
    if (settingsStatus_ &&
        settingsStatus_->severity == StatusSeverity::Success &&
        settingsStatus_->text == L"All changes saved.") {
      settingsStatus_.reset();
    }
    const auto effects = feathercast::ui::SettingsController::Open(
        settingsState_, SettingsCategoryIndex(settingsCategory_));
    shortcutRecorder_.Reset();
    ShowSettingsWindow();
    ExecuteSettingsEffects(effects);
  }

  feathercast::library_ui::ManagerData LibraryManagerData() {
    feathercast::library_ui::ManagerData data;
    {
      std::lock_guard lock(dataMutex_);
      data.snippets = snippets_;
      for (const auto& app : apps_) {
        if (!app.id.empty() && !app.name.empty()) {
          data.availableApps.push_back({app.id, app.name});
        }
      }
    }
    data.quicklinks = settings_.quicklinks;
    for (const auto& [appId, alias] : settings_.appAliases) {
      const auto app = std::find_if(
          data.availableApps.begin(), data.availableApps.end(),
          [&](const auto& choice) { return choice.id == appId; });
      data.appAliases.push_back(
          {appId, app == data.availableApps.end() ? L"" : app->name, alias});
    }
    for (const auto& [keyword, urlTemplate] : settings_.searchEngines) {
      data.webSearches.push_back({keyword, urlTemplate});
    }
    std::sort(data.availableApps.begin(), data.availableApps.end(),
              [](const auto& left, const auto& right) {
                return feathercast::library::NormalizeKeyword(left.name) <
                       feathercast::library::NormalizeKeyword(right.name);
              });
    data.snippetsWritable = snippetsWritable_;
    data.quicklinksWritable = !settingsPersistenceBlocked_;
    data.settingsWritable = !settingsPersistenceBlocked_;
    data.snippetsMessage = snippetsLoadMessage_;
    if (settingsPersistenceBlocked_) {
      data.quicklinksMessage =
          L"settings.json is protected from automatic writes. Quicklink editing is disabled.";
      data.settingsMessage =
          L"settings.json is protected from automatic writes. Customization is disabled.";
    }
    return data;
  }

  feathercast::library::OperationResult SaveManagedSnippets(
      const std::vector<feathercast::snippets::Snippet>& candidate) {
    if (!snippetsWritable_) {
      return {false, snippetsLoadMessage_.empty()
                         ? L"Snippet editing is unavailable."
                         : snippetsLoadMessage_};
    }
    const auto saved = feathercast::snippets_io::Save(
        SnippetsPath(), candidate, snippetsFingerprint_);
    if (!saved.succeeded) {
      snippetsFingerprint_ = saved.fingerprint;
      snippetsWritable_ = false;
      snippetsLoadMessage_ = saved.message;
      return {false, saved.message};
    }
    {
      std::lock_guard lock(dataMutex_);
      snippets_ = candidate;
    }
    snippetsFingerprint_ = saved.fingerprint;
    snippetsWritable_ = true;
    snippetsLoadMessage_.clear();
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"Snippets saved."};
  }

  feathercast::library::OperationResult SaveManagedQuicklinks(
      const std::vector<Quicklink>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false,
              L"settings.json is protected from automatic writes. Quicklink editing is disabled."};
    }
    Settings next = settings_;
    next.quicklinks = candidate;
    std::wstring error;
    if (!persistence_.SaveSettingsAndWait(next, &error)) {
      return {false, error.empty() ? L"Could not save settings.json." : error};
    }
    settings_ = std::move(next);
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"Quicklinks saved."};
  }

  feathercast::library::OperationResult SaveManagedAppAliases(
      const std::vector<feathercast::library::AppAlias>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false, L"settings.json is protected from automatic writes."};
    }
    Settings next = settings_;
    next.appAliases.clear();
    for (std::size_t index = 0; index < candidate.size(); ++index) {
      const auto& entry = candidate[index];
      if (const auto error = feathercast::library::ValidateAppAlias(
              entry, candidate, index)) {
        return {false, *error};
      }
      next.appAliases[entry.appId] = entry.alias;
    }
    std::wstring error;
    if (!persistence_.SaveSettingsAndWait(next, &error)) {
      return {false, error.empty() ? L"Could not save settings.json." : error};
    }
    settings_ = std::move(next);
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"App aliases saved."};
  }

  feathercast::library::OperationResult SaveManagedWebSearches(
      const std::vector<feathercast::library::WebSearch>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false, L"settings.json is protected from automatic writes."};
    }
    Settings next = settings_;
    next.searchEngines.clear();
    for (std::size_t index = 0; index < candidate.size(); ++index) {
      if (const auto error = feathercast::library::ValidateWebSearch(
              candidate[index], candidate, index)) {
        return {false, *error};
      }
      next.searchEngines[feathercast::library::NormalizeKeyword(
          candidate[index].keyword)] = candidate[index].urlTemplate;
    }
    std::wstring error;
    if (!persistence_.SaveSettingsAndWait(next, &error)) {
      return {false, error.empty() ? L"Could not save settings.json." : error};
    }
    settings_ = std::move(next);
    RequestSearch();
    return {true, L"Web searches saved."};
  }

  feathercast::library::OperationResult RestoreDefaultWebSearches() {
    std::vector<feathercast::library::WebSearch> defaults;
    for (const auto& [keyword, urlTemplate] :
         feathercast::settings::DefaultSearchEngines()) {
      defaults.push_back({keyword, urlTemplate});
    }
    return SaveManagedWebSearches(defaults);
  }

  feathercast::library_ui::ManagerData ReloadLibraryManagerData() {
    const auto loaded = feathercast::snippets_io::Load(SnippetsPath());
    snippetsFingerprint_ = loaded.fingerprint;
    snippetsWritable_ = loaded.Writable();
    snippetsLoadMessage_ = loaded.message;
    if (loaded.Writable()) {
      {
        std::lock_guard lock(dataMutex_);
        snippets_ = loaded.snippets;
      }
      MarkSearchDataChanged();
      RequestSearch();
    }
    return LibraryManagerData();
  }

  void OpenSnippetsFileForLibrary() {
    EnsureSnippetsFile();
    ShellExecuteW(nullptr, L"open", SnippetsPath().c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }

  void OpenLibraryManager(feathercast::library::ItemKind initialKind,
                          std::wstring initialAppId = {}) {
    feathercast::library_ui::ManagerCallbacks callbacks;
    callbacks.saveSnippets = [this](const auto& snippets) {
      return SaveManagedSnippets(snippets);
    };
    callbacks.saveQuicklinks = [this](const auto& quicklinks) {
      return SaveManagedQuicklinks(quicklinks);
    };
    callbacks.saveAppAliases = [this](const auto& aliases) {
      return SaveManagedAppAliases(aliases);
    };
    callbacks.saveWebSearches = [this](const auto& searches) {
      return SaveManagedWebSearches(searches);
    };
    callbacks.restoreDefaultWebSearches = [this] {
      return RestoreDefaultWebSearches();
    };
    callbacks.reload = [this] { return ReloadLibraryManagerData(); };
    callbacks.openSnippetsFile = [this] { OpenSnippetsFileForLibrary(); };
    HWND owner = settingsHwnd_ && IsWindowVisible(settingsHwnd_)
                     ? settingsHwnd_
                     : hwnd_;
    feathercast::library_ui::ShowLibraryManager(
        owner, LibraryManagerData(), std::move(callbacks), initialKind,
        std::move(initialAppId));
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void ShowSettingsWindow() {
    if (!settingsHwnd_) return;
    EnterInteractiveScheduling();
    settingsClosing_ = false;
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    const float scale = GetMonitorScale(monitor);

    const int width = static_cast<int>(SETTINGS_WIDTH * scale);
    const int height = static_cast<int>(SETTINGS_HEIGHT * scale);

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const int maxHeight = mi.rcWork.bottom - mi.rcWork.top - static_cast<int>(40 * scale);
    const int clamped = std::min(height, maxHeight);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
    const int yPos = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - clamped) / 2;

    SetWindowPos(settingsHwnd_, HWND_NOTOPMOST, x, yPos, width, clamped, SWP_NOACTIVATE);
    // Reassert the transparent window treatment now that the window is sized.
    ApplyGlass(settingsHwnd_);
    settingsCategoryTop_.Snap(
        kSettTop + static_cast<double>(SettingsCategoryIndex(settingsCategory_)) *
                       kSettCategoryRow);
    settingsVisualScroll_.Snap(settingsScroll_);
    const bool surfaceReady =
        PrewarmGlassSurface(settingsHwnd_, settingsSurface_);
    if (surfaceReady) {
      StartSurfaceOpen(settingsHwnd_, settingsBounds_, settingsOpacity_, false);
    } else {
      RECT windowBounds{};
      GetWindowRect(settingsHwnd_, &windowBounds);
      SnapWindowBounds(settingsBounds_, windowBounds);
      settingsOpacity_.Snap(1.0);
    }
    RedrawWindow(settingsHwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    ShowWindow(settingsHwnd_, SW_SHOW);
    SetForegroundWindow(settingsHwnd_);
    SetActiveWindow(settingsHwnd_);
    SetFocus(settingsHwnd_);
    UpdateBackgroundState();
    RestartAnimationClock();
  }

  void FinishHideSettings() {
    settingsClosing_ = false;
    CancelPointerPress(settingsHwnd_);
    const auto effects =
        feathercast::ui::SettingsController::Close(settingsState_);
    shortcutRecorder_.Reset();
    ExecuteSettingsEffects(effects);
    UpdateBackgroundState();
  }

  void HideSettings(bool animate = true) {
    if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) return;
    if (animate && FadeAnimationsAllowed() && !settingsClosing_) {
      settingsClosing_ = true;
      CancelPointerPress(settingsHwnd_);
      settingsOpacity_.Retarget(0.0, kSurfaceCloseSeconds, true);
      RequestAnimationFrame();
      return;
    }
    FinishHideSettings();
  }

  void EnterActionMode(const DisplayItem& target) {
    if (target.isCommand ||
        (target.isAction && target.action != ActionKind::ArrangeWindow) ||
        target.isExtension ||
        target.isRunCommand || target.isWebSearch || target.isCapability) {
      return;
    }
    const std::wstring selectedKey =
        selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())
            ? flatItems_[selected_].Key()
            : std::wstring{};
    const auto effects = feathercast::ui::OverlayController::EnterAction(
        overlayState_, target, selectedKey);
    SyncSelectionAnimationToTarget();
    ExecuteOverlayEffects(effects);
  }

  void ExitActionMode() {
    RestoreNavigationState();
  }

  // Open a dedicated result browse view in place of the result list.
  void EnterBrowseView(BrowseView view) {
    const std::wstring selectedKey =
        selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())
            ? flatItems_[selected_].Key()
            : std::wstring{};
    const auto effects = feathercast::ui::OverlayController::EnterBrowse(
        overlayState_, view, selectedKey);
    SyncSelectionAnimationToTarget();
    ExecuteOverlayEffects(effects);
  }

  void ExitBrowseView() {
    RestoreNavigationState();
  }

  void RestoreNavigationState() {
    const auto effects =
        feathercast::ui::OverlayController::RestoreNavigation(overlayState_);
    SyncSelectionAnimationToTarget();
    ExecuteOverlayEffects(effects);
  }

  // Placeholder shown in the search box when the query is empty.
  std::wstring SearchPlaceholder() const {
    if (browseView_ == BrowseView::Clipboard) return L"Search clipboard history...";
    if (browseView_ == BrowseView::Emoji) return L"Search emoji...";
    if (browseView_ == BrowseView::Games) return L"Search installed games...";
    if (browseView_ == BrowseView::Capabilities) return L"Search FeatherCast features...";
    if (actionMode_) return L"Actions for " + actionTarget_.Name();
    return L"Search apps, files, commands, and more...";
  }

  std::wstring EmptyResultsMessage() const {
    if (SearchPending()) return L"Searching...";
    const bool emptyQuery = Trim(query_).empty();
    const auto scopedQuery = feathercast::search_scope::Parse(query_);
    if (browseView_ == BrowseView::Clipboard) {
      if (!settings_.clipboardHistoryEnabled) return L"Clipboard history is turned off";
      return emptyQuery ? L"Clipboard history is empty"
                        : L"No matching clipboard items";
    }
    if (browseView_ == BrowseView::Emoji) {
      return emptyQuery ? L"No emoji available" : L"No matching emoji";
    }
    if (browseView_ == BrowseView::Games) {
      return emptyQuery ? L"No installed games found" : L"No matching games";
    }
    if (browseView_ == BrowseView::Capabilities) {
      return emptyQuery ? L"No features available"
                        : L"No matching features — try apps, calculator, clipboard, or shortcuts";
    }
    if (actionMode_) {
      return emptyQuery ? L"No actions available" : L"No matching actions";
    }
    if (scopedQuery.scope == feathercast::search_scope::Scope::Files) {
      if (!settings_.fileIndexEnabled) {
        return L"File indexing is turned off in Privacy settings";
      }
      return scopedQuery.terms.empty() ? L"No indexed files in the selected folders"
                                       : L"No matching indexed files";
    }
    if (scopedQuery.scope == feathercast::search_scope::Scope::Games) {
      return scopedQuery.terms.empty() ? L"No installed games found"
                                       : L"No matching games";
    }
    if (!appsReady_.load(std::memory_order_acquire)) return L"Loading apps...";
    return emptyQuery ? L"No items available"
                      : L"No results — try Discover FeatherCast for examples";
  }

  void ClearQuery() {
    feathercast::ui::OverlayController::SetQuery(overlayState_, L"");
  }

  void SetQueryText(std::wstring value) {
    navigationStack_.clear();
    pendingNavigationRestore_.reset();
    actionMode_ = false;
    browseView_ = BrowseView::None;
    const auto effects = feathercast::ui::OverlayController::SetQuery(
        overlayState_, std::move(value));
    SyncSelectionAnimationToTarget();
    ExecuteOverlayEffects(effects);
  }

  void ClampCaret() {
    feathercast::ui::OverlayController::ClampCaret(overlayState_);
  }

  std::optional<std::pair<size_t, size_t>> SelectionRange() const {
    return feathercast::ui::OverlayController::SelectionRange(overlayState_);
  }

  void MoveCaret(size_t next, bool extendSelection) {
    feathercast::ui::OverlayController::MoveCaret(
        overlayState_, next, extendSelection);
  }

  bool DeleteSelection() {
    return feathercast::ui::OverlayController::DeleteSelection(overlayState_);
  }

  void CopySelectionToClipboard() {
    if (const auto range = SelectionRange()) {
      CopyTextToClipboard(query_.substr(range->first, range->second - range->first));
    }
  }

  void InsertQueryText(const std::wstring& text) {
    feathercast::ui::OverlayController::InsertText(overlayState_, text);
  }

  void SetCaretFromSearchClick(float x) {
    ClampCaret();
    if (query_.empty() || !dwriteFactory_) {
      caret_ = query_.size();
      selectionAnchor_.reset();
      return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetWindowScale(hwnd_);
    const float width = static_cast<float>(rc.right - rc.left) / scale;
    const float textLeft = 52.0f;
    const float textWidth = std::max(1.0f, width - 94.0f - textLeft);
    if (x <= textLeft) {
      caret_ = 0;
      selectionAnchor_.reset();
      return;
    }
    if (x >= textLeft + textWidth) {
      caret_ = query_.size();
      selectionAnchor_.reset();
      return;
    }

    ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = dwriteFactory_->CreateTextLayout(
        query_.c_str(),
        static_cast<UINT32>(query_.size()),
        inputFormat_.Get(),
        textWidth,
        48.0f,
        layout.GetAddressOf());
    if (FAILED(hr)) {
      caret_ = query_.size();
      selectionAnchor_.reset();
      return;
    }

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (SUCCEEDED(layout->HitTestPoint(x - textLeft, 24.0f, &trailing, &inside, &metrics))) {
      caret_ = static_cast<size_t>(metrics.textPosition + (trailing ? metrics.length : 0));
      ClampCaret();
      selectionAnchor_.reset();
    }
  }

  float GetWindowScale(HWND hwnd) const {
    UINT dpi = 96;
    typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);
    if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
      if (auto proc = reinterpret_cast<GetDpiForWindowProc>(GetProcAddress(hUser32, "GetDpiForWindow"))) {
        dpi = proc(hwnd);
      }
    }
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
  }

  float GetMonitorScale(HMONITOR hMonitor) const {
    UINT dpiX = 96;
    UINT dpiY = 96;
    typedef HRESULT(WINAPI* GetDpiForMonitorProc)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE hShcore = GetModuleHandleW(L"shcore.dll")) {
      if (auto proc = reinterpret_cast<GetDpiForMonitorProc>(GetProcAddress(hShcore, "GetDpiForMonitor"))) {
        proc(hMonitor, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
      }
    } else if (HMODULE hShcoreLoad = LoadLibraryW(L"shcore.dll")) {
      if (auto proc = reinterpret_cast<GetDpiForMonitorProc>(GetProcAddress(hShcoreLoad, "GetDpiForMonitor"))) {
        proc(hMonitor, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
      }
      FreeLibrary(hShcoreLoad);
    }
    return dpiX > 0 ? static_cast<float>(dpiX) / 96.0f : 1.0f;
  }

  void PositionWindow() {
    const MONITORINFO mi = OverlayMonitorInfo();
    HMONITOR monitor = overlayMonitor_;
    if (!monitor) monitor = ResolveOverlayMonitor(GetForegroundWindow());
    const float scale = GetMonitorScale(monitor);

    const int width = static_cast<int>(OverlayWidth() * scale);
    const int height = static_cast<int>(CurrentHeight() * scale);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
    const int y = mi.rcWork.top + static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.22);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  }

  static constexpr float kResultsTop = 60.0f;
  static constexpr float kSectionHeaderHeight = 26.0f;
  static constexpr float kResultRowHeight = 50.0f;
  static constexpr float kResultRowGap = 2.0f;
  static constexpr float kResultRowStride = kResultRowHeight + kResultRowGap;

  int ResultsContentHeight() const {
    int height = 0;
    for (const auto& section : sections_) {
      height += static_cast<int>(kSectionHeaderHeight);
      // 52px per row: 50px row height + 2px gap (up from 48px = 46px + 2px gap).
      height += static_cast<int>(section.items.size() * kResultRowStride);
    }
    return height;
  }

  int CurrentHeight() const {
    if (confirmation_) return settings_.compactMode ? 300 : WIN_HEIGHT;
    if (!settings_.compactMode) return WIN_HEIGHT;
    if (Trim(query_).empty() && !actionMode_ && browseView_ == BrowseView::None) return COMPACT_BASE_HEIGHT;
    const MONITORINFO mi = OverlayMonitorInfo();
    const int maxHeight = static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.7);
    return std::clamp(COMPACT_BASE_HEIGHT + ResultsContentHeight(), COMPACT_BASE_HEIGHT, maxHeight);
  }

  void ApplyWindowSize() {
    const float scale = GetWindowScale(hwnd_);
    const int height = static_cast<int>(CurrentHeight() * scale);
    const int width = static_cast<int>(OverlayWidth() * scale);
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    const int currentWidth = rc.right - rc.left;
    const int currentHeight = rc.bottom - rc.top;
    if (currentWidth != width || currentHeight != height) {
      if (IsWindowVisible(hwnd_) && SpatialAnimationsAllowed()) {
        if (!overlayBounds_.Active()) SnapWindowBounds(overlayBounds_, rc);
        overlayBounds_.Retarget(rc.left, rc.top, width, height,
                                kOverlayResizeSeconds, true);
        RequestAnimationFrame();
      } else {
        SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top, width, height,
                     SWP_NOACTIVATE | SWP_NOMOVE);
        RECT applied{};
        GetWindowRect(hwnd_, &applied);
        SnapWindowBounds(overlayBounds_, applied);
        ApplyRoundedRegion(width, height);
      }
    }
  }

  LONGLONG NowQpc() const {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
  }

  enum class SelectionMotion {
    Keyboard,
    Hover,
  };

  static constexpr double kSelectionHoverSettleSeconds = 0.055;
  static constexpr double kSelectionKeyboardSettleSeconds = 0.090;
  static constexpr double kSurfaceOpenSeconds = 0.150;
  static constexpr double kSurfaceCloseSeconds = 0.090;
  static constexpr double kOverlayResizeSeconds = 0.140;
  static constexpr double kSettingsResizeSeconds = 0.160;
  static constexpr double kOverlayScrollSeconds = 0.085;
  static constexpr double kSettingsScrollSeconds = 0.090;
  static constexpr double kResultTransitionSeconds = 0.110;

  double ConsumeAnimationDeltaSeconds() {
    if (!qpcFrequency_.QuadPart) return 0.0;
    const LONGLONG now = NowQpc();
    if (!lastAnimationFrameQpc_) {
      lastAnimationFrameQpc_ = now;
      return 0.0;
    }
    const double dt = static_cast<double>(now - lastAnimationFrameQpc_) / static_cast<double>(qpcFrequency_.QuadPart);
    lastAnimationFrameQpc_ = now;
    return std::clamp(dt, 0.0, 0.05);
  }

  void SnapWindowBounds(feathercast::motion::AnimatedBounds& bounds,
                        const RECT& rect) {
    bounds.Snap(rect.left, rect.top, rect.right - rect.left,
                rect.bottom - rect.top);
  }

  void ApplyAnimatedBounds(HWND hwnd,
                           feathercast::motion::AnimatedBounds& bounds) {
    if (!hwnd) return;
    SetWindowPos(
        hwnd, nullptr,
        static_cast<int>(std::lround(bounds.left.Value())),
        static_cast<int>(std::lround(bounds.top.Value())),
        std::max(1, static_cast<int>(std::lround(bounds.width.Value()))),
        std::max(1, static_cast<int>(std::lround(bounds.height.Value()))),
        SWP_NOZORDER | SWP_NOACTIVATE);
    if (surfaceResizeFailed_) {
      surfaceResizeFailed_ = false;
      bounds.Snap(bounds.left.Target(), bounds.top.Target(),
                  bounds.width.Target(), bounds.height.Target());
      SetWindowPos(
          hwnd, nullptr,
          static_cast<int>(std::lround(bounds.left.Value())),
          static_cast<int>(std::lround(bounds.top.Value())),
          std::max(1, static_cast<int>(std::lround(bounds.width.Value()))),
          std::max(1, static_cast<int>(std::lround(bounds.height.Value()))),
          SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

  void StartSurfaceOpen(HWND hwnd,
                        feathercast::motion::AnimatedBounds& bounds,
                        feathercast::motion::ScalarAnimation& opacity,
                        bool topAnchored) {
    if (!hwnd) return;
    RECT target{};
    GetWindowRect(hwnd, &target);
    if (!FadeAnimationsAllowed() && !SpatialAnimationsAllowed()) {
      SnapWindowBounds(bounds, target);
      opacity.Snap(1.0);
      return;
    }

    const double targetWidth = target.right - target.left;
    const double targetHeight = target.bottom - target.top;
    if (SpatialAnimationsAllowed()) {
      const auto initial = feathercast::motion::OpeningBounds(
          {static_cast<double>(target.left), static_cast<double>(target.top),
           targetWidth, targetHeight},
          0.98, topAnchored);
      bounds.Snap(initial.left, initial.top, initial.width, initial.height);
      SetWindowPos(hwnd, nullptr, static_cast<int>(std::lround(initial.left)),
                   static_cast<int>(std::lround(initial.top)),
                   static_cast<int>(std::lround(initial.width)),
                   static_cast<int>(std::lround(initial.height)),
                   SWP_NOZORDER | SWP_NOACTIVATE);
      bounds.Retarget(target.left, target.top, targetWidth, targetHeight,
                      kSurfaceOpenSeconds, true);
    } else {
      SnapWindowBounds(bounds, target);
    }
    if (FadeAnimationsAllowed()) {
      opacity.Snap(0.0);
      opacity.Retarget(1.0, kSurfaceOpenSeconds, true);
    } else {
      opacity.Snap(1.0);
    }
    RequestAnimationFrame();
  }

  void RetargetOverlayScroll(bool animate = true) {
    overlayVisualScroll_.Retarget(
        static_cast<double>(scroll_), kOverlayScrollSeconds,
        animate && SpatialAnimationsAllowed());
    if (overlayVisualScroll_.Active()) RequestAnimationFrame();
  }

  void RetargetSettingsScroll(bool animate = true) {
    settingsVisualScroll_.Retarget(
        static_cast<double>(settingsScroll_), kSettingsScrollSeconds,
        animate && SpatialAnimationsAllowed());
    if (settingsVisualScroll_.Active()) RequestAnimationFrame();
  }

  void SnapSpatialMotionToTargets() {
    auto snap = [](feathercast::motion::ScalarAnimation& animation) {
      animation.Snap(animation.Target());
    };
    snap(overlayVisualScroll_);
    snap(settingsVisualScroll_);
    snap(settingsCategoryTop_);
    snap(volumeVisualPercent_);
    for (auto& [_, element] : resultRowMotion_) {
      snap(element.offsetY);
    }
    for (auto& [_, element] : resultHeaderMotion_) {
      snap(element.offsetY);
    }
    overlayBounds_.Snap(overlayBounds_.left.Target(),
                        overlayBounds_.top.Target(),
                        overlayBounds_.width.Target(),
                        overlayBounds_.height.Target());
    settingsBounds_.Snap(settingsBounds_.left.Target(),
                         settingsBounds_.top.Target(),
                         settingsBounds_.width.Target(),
                         settingsBounds_.height.Target());
    volumeBounds_.Snap(volumeBounds_.left.Target(), volumeBounds_.top.Target(),
                       volumeBounds_.width.Target(),
                       volumeBounds_.height.Target());
    auto applySnapped = [](HWND hwnd,
                           const feathercast::motion::AnimatedBounds& bounds) {
      if (!hwnd || !IsWindowVisible(hwnd) || bounds.width.Value() <= 0.0 ||
          bounds.height.Value() <= 0.0) {
        return;
      }
      SetWindowPos(
          hwnd, nullptr, static_cast<int>(std::lround(bounds.left.Value())),
          static_cast<int>(std::lround(bounds.top.Value())),
          static_cast<int>(std::lround(bounds.width.Value())),
          static_cast<int>(std::lround(bounds.height.Value())),
          SWP_NOZORDER | SWP_NOACTIVATE);
    };
    applySnapped(hwnd_, overlayBounds_);
    applySnapped(settingsHwnd_, settingsBounds_);
    applySnapped(volumeHwnd_, volumeBounds_);
    if (animatingSelection_) SyncSelectionAnimationToTarget();
  }

  void SnapAllMotionToTargets() {
    auto snap = [](feathercast::motion::ScalarAnimation& animation) {
      animation.Snap(animation.Target());
    };
    SnapSpatialMotionToTargets();
    snap(overlayOpacity_);
    snap(settingsOpacity_);
    snap(volumeOpacity_);
    snap(confirmationProgress_);
    snap(settingsPageProgress_);
    for (auto& [_, animation] : switchAnimations_) snap(animation);
    for (auto& [_, element] : resultRowMotion_) snap(element.opacity);
    for (auto& [_, element] : resultHeaderMotion_) snap(element.opacity);
    animating_ = false;
    overlayRevealTimeline_.Reset();
    if (confirmationClosing_) FinishCancelConfirmation();
    if (overlayClosing_ && pendingOverlayClose_) {
      FinishHideOverlay(*pendingOverlayClose_);
    }
    if (settingsClosing_) FinishHideSettings();
    if (volumeClosing_) FinishHideVolumeControl(pendingVolumeRestore_);
  }

  std::optional<float> SelectedRowTop() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(flatItems_.size())) return std::nullopt;
    float y = kResultsTop -
              static_cast<float>(overlayVisualScroll_.Value());
    int row = 0;
    for (const auto& section : sections_) {
      y += kSectionHeaderHeight;
      for (size_t i = 0; i < section.items.size(); ++i) {
        if (row == selected_) {
          if (const auto motion =
                  resultRowMotion_.find(section.items[i].Key());
              motion != resultRowMotion_.end()) {
            y += static_cast<float>(motion->second.offsetY.Value());
          }
          return y;
        }
        y += kResultRowStride;
        ++row;
      }
    }
    return std::nullopt;
  }

  void SyncSelectionAnimationToTarget() {
    const auto targetY = SelectedRowTop();
    visualSelectedY_ = targetY.value_or(-1.0f);
    animatingSelection_ = false;
    StopSelectionAnimationTimer();
  }

  void StartSelectionAnimationFrom(std::optional<float> previousY, int previousScroll, SelectionMotion motion) {
    const auto targetY = SelectedRowTop();
    if (!SpatialAnimationsAllowed() || !previousY || !targetY) {
      SyncSelectionAnimationToTarget();
      if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    selectionSettleSeconds_ = motion == SelectionMotion::Hover ? kSelectionHoverSettleSeconds : kSelectionKeyboardSettleSeconds;
    if (!animatingSelection_ || visualSelectedY_ < 0.0f) {
      visualSelectedY_ = *previousY - static_cast<float>(scroll_ - previousScroll);
    }
    animatingSelection_ = true;
    StartSelectionAnimationTimer();
  }

  void SelectResult(int next, bool animate, bool ensureVisible, SelectionMotion motion = SelectionMotion::Keyboard) {
    if (!ResultsActivationAllowed()) return;
    if (flatItems_.empty()) {
      feathercast::ui::OverlayController::Select(
          overlayState_, 0, 0, false);
      SyncSelectionAnimationToTarget();
      if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }

    next = std::clamp(next, 0, static_cast<int>(flatItems_.size()) - 1);
    if (next == selected_) {
      if (ensureVisible) {
        EnsureSelectedVisible();
        SyncSelectionAnimationToTarget();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return;
    }

    const auto previousY = SelectedRowTop();
    const int previousScroll = scroll_;
    feathercast::ui::OverlayController::Select(
        overlayState_, next, static_cast<int>(flatItems_.size()), false);
    if (ensureVisible) EnsureSelectedVisible();
    if (animate) {
      StartSelectionAnimationFrom(previousY, previousScroll, motion);
    } else {
      SyncSelectionAnimationToTarget();
      if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, selected_ + 2);
    ScheduleSelectedPreview();
  }

  bool SelectedFilePath(std::filesystem::path* path = nullptr) const {
    if (selected_ < 0 || selected_ >= static_cast<int>(flatItems_.size())) {
      return false;
    }
    const auto& item = flatItems_[static_cast<std::size_t>(selected_)];
    if (item.isWindow || item.isCommand || item.isAction ||
        item.app.source != L"file" || item.app.path.empty()) {
      return false;
    }
    if (path) *path = item.app.path;
    return true;
  }

  void ClosePreview() {
    previewOpen_ = false;
    ++previewGeneration_;
    previewService_.Invalidate(previewGeneration_);
    previewResult_.reset();
    previewBitmap_.Reset();
    previewScroll_ = 0.0f;
    KillTimer(hwnd_, TIMER_PREVIEW_LOAD);
    ApplyWindowSize();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void TogglePreview() {
    if (previewOpen_) {
      ClosePreview();
      return;
    }
    if (!SelectedFilePath()) {
      SetOverlayStatus(StatusSeverity::Info,
                       L"Select an indexed file or folder to preview.");
      return;
    }
    previewOpen_ = true;
    previewResult_.reset();
    previewBitmap_.Reset();
    ApplyWindowSize();
    ScheduleSelectedPreview();
  }

  void ScheduleSelectedPreview() {
    if (!previewOpen_) return;
    previewResult_.reset();
    previewBitmap_.Reset();
    KillTimer(hwnd_, TIMER_PREVIEW_LOAD);
    SetTimer(hwnd_, TIMER_PREVIEW_LOAD, 120, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void LoadSelectedPreview() {
    if (!previewOpen_) return;
    std::filesystem::path path;
    if (!SelectedFilePath(&path)) {
      previewResult_.reset();
      previewBitmap_.Reset();
      return;
    }
    feathercast::preview::Request request;
    request.generation = ++previewGeneration_;
    request.path = std::move(path);
    request.terms = feathercast::search_scope::Parse(query_).terms;
    if (!previewService_.Load(std::move(request))) {
      ReportBackgroundFailure(L"The preview worker is unavailable.");
    }
  }

  void UpdateSelectionAnimation(double deltaTime) {
    const auto targetY = SelectedRowTop();
    if (!SpatialAnimationsAllowed() || !targetY) {
      SyncSelectionAnimationToTarget();
      return;
    }
    if (visualSelectedY_ < 0.0f) {
      visualSelectedY_ = *targetY;
      animatingSelection_ = false;
      return;
    }

    const float dy = *targetY - visualSelectedY_;
    if (std::abs(dy) > 0.5f) {
      const double settle = std::max(selectionSettleSeconds_, 0.001);
      const float blend = static_cast<float>(1.0 - std::pow(0.001, deltaTime / settle));
      visualSelectedY_ += dy * blend;
      animatingSelection_ = true;
    } else {
      visualSelectedY_ = *targetY;
      animatingSelection_ = false;
    }
  }

  bool HasRevealAnimation() const {
    return visible_ && FadeAnimationsAllowed() && animating_;
  }

  template <typename ElementMap>
  bool HasElementMotion(const ElementMap& elements) const {
    return std::any_of(elements.begin(), elements.end(), [](const auto& entry) {
      return entry.second.offsetY.Active() || entry.second.opacity.Active();
    });
  }

  bool HasActiveMotion() const {
    const bool switchMotion =
        std::any_of(switchAnimations_.begin(), switchAnimations_.end(),
                    [](const auto& entry) { return entry.second.Active(); });
    return HasRevealAnimation() || animatingSelection_ ||
           overlayOpacity_.Active() || settingsOpacity_.Active() ||
           volumeOpacity_.Active() || confirmationProgress_.Active() ||
           overlayVisualScroll_.Active() || settingsVisualScroll_.Active() ||
           settingsPageProgress_.Active() ||
           settingsCategoryTop_.Active() || volumeVisualPercent_.Active() ||
           overlayBounds_.Active() || settingsBounds_.Active() ||
           volumeBounds_.Active() || HasElementMotion(resultRowMotion_) ||
           HasElementMotion(resultHeaderMotion_) || switchMotion;
  }

  void RequestAnimationFrame() {
    if (!hwnd_ || animationFrameQueued_ || !HasActiveMotion()) return;
    if (!animationLoopRunning_) {
      lastAnimationFrameQpc_ = NowQpc();
      animationLoopRunning_ = true;
    }
    animationFrameQueued_ = PostMessageW(hwnd_, WM_ANIMATION_FRAME, 0, 0) != FALSE;
  }

  void RestartAnimationClock() {
    if (!HasActiveMotion()) return;
    lastAnimationFrameQpc_ = NowQpc();
    animationLoopRunning_ = true;
    RequestAnimationFrame();
  }

  void OnAnimationFrame() {
    animationFrameQueued_ = false;
    if (!HasActiveMotion()) {
      animationLoopRunning_ = false;
      return;
    }

    const double deltaTime = ConsumeAnimationDeltaSeconds();
    if (animatingSelection_) UpdateSelectionAnimation(deltaTime);
    if (animating_) {
      overlayRevealTimeline_.Update(deltaTime);
      if (!overlayRevealTimeline_.Active()) animating_ = false;
    }
    overlayOpacity_.Update(deltaTime);
    settingsOpacity_.Update(deltaTime);
    volumeOpacity_.Update(deltaTime);
    confirmationProgress_.Update(deltaTime);
    overlayVisualScroll_.Update(deltaTime);
    settingsVisualScroll_.Update(deltaTime);
    settingsPageProgress_.Update(deltaTime);
    settingsCategoryTop_.Update(deltaTime);
    volumeVisualPercent_.Update(deltaTime);
    const bool overlayBoundsChanged = overlayBounds_.Active();
    const bool settingsBoundsChanged = settingsBounds_.Active();
    const bool volumeBoundsChanged = volumeBounds_.Active();
    overlayBounds_.Update(deltaTime);
    settingsBounds_.Update(deltaTime);
    volumeBounds_.Update(deltaTime);
    for (auto& [_, motion] : resultRowMotion_) {
      motion.offsetY.Update(deltaTime);
      motion.opacity.Update(deltaTime);
    }
    for (auto& [_, motion] : resultHeaderMotion_) {
      motion.offsetY.Update(deltaTime);
      motion.opacity.Update(deltaTime);
    }
    for (auto& [_, animation] : switchAnimations_) {
      animation.Update(deltaTime);
    }

    if (overlayBoundsChanged) ApplyAnimatedBounds(hwnd_, overlayBounds_);
    if (settingsBoundsChanged) {
      ApplyAnimatedBounds(settingsHwnd_, settingsBounds_);
    }
    if (volumeBoundsChanged) ApplyAnimatedBounds(volumeHwnd_, volumeBounds_);

    if (confirmationClosing_ && !confirmationProgress_.Active() &&
        confirmationProgress_.Value() <= 0.001) {
      FinishCancelConfirmation();
    }
    if (overlayClosing_ && pendingOverlayClose_ &&
        !overlayOpacity_.Active() && overlayOpacity_.Value() <= 0.001) {
      FinishHideOverlay(*pendingOverlayClose_);
    }
    if (settingsClosing_ && !settingsOpacity_.Active() &&
        settingsOpacity_.Value() <= 0.001) {
      FinishHideSettings();
    }
    if (volumeClosing_ && !volumeOpacity_.Active() &&
        volumeOpacity_.Value() <= 0.001) {
      FinishHideVolumeControl(pendingVolumeRestore_);
    }

    if (visible_) {
      RedrawWindow(hwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    if (settingsHwnd_ && IsWindowVisible(settingsHwnd_)) {
      RedrawWindow(settingsHwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    if (volumeVisible_) {
      RedrawWindow(volumeHwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    if (HasActiveMotion()) {
      // Wait until DWM has consumed this frame before queueing another one.
      // This keeps the self-posted animation loop display-paced even when the
      // process has just returned from reduced background priority.
      DwmFlush();
      RequestAnimationFrame();
    } else {
      animationLoopRunning_ = false;
    }
  }

  void StartSelectionAnimationTimer() {
    if (!hwnd_ || !visible_ || !SpatialAnimationsAllowed() ||
        !animatingSelection_) {
      return;
    }
    RequestAnimationFrame();
  }

  void StopSelectionAnimationTimer() {
    animatingSelection_ = false;
  }

  int OverlayWidth() const {
    const int base = std::clamp(settings_.overlayWidth, MIN_OVERLAY_WIDTH,
                                MAX_OVERLAY_WIDTH);
    if (!previewOpen_) return base;
    const MONITORINFO monitor = OverlayMonitorInfo();
    HMONITOR handle = overlayMonitor_;
    if (!handle) handle = ResolveOverlayMonitor(GetForegroundWindow());
    const float scale = GetMonitorScale(handle);
    const int available = static_cast<int>(
        (monitor.rcWork.right - monitor.rcWork.left) / scale) - 32;
    return std::max(base, std::min(base + 420, available));
  }

  // Visible corners are drawn by Direct2D. This only ensures no stale Win32 region is
  // left on the window, because a region would fight DirectComposition alpha.
  void ApplyRoundedRegion(int /*width*/, int /*height*/) {
    SetWindowRgn(hwnd_, nullptr, TRUE);
  }

  COLORREF ActiveAccent() const {
    if (!settings_.syncAccentColor) return ColorRefFromHex(settings_.customAccentColor);
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
      return RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
    }
    return ColorRefFromTheme(theme_.accentFallback);
  }

  void Paint() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    if (EnsureRenderResources() && CreateGlassSurface(overlaySurface_, hwnd_)) {
      ID2D1DeviceContext* dc = overlaySurface_.dc.Get();
      SetActiveTarget(dc);
      const HRESULT brushes = EnsureBrushResources();
      if (FAILED(brushes)) {
        activeRT_ = nullptr;
        activeDC_.Reset();
        ScheduleRenderRecovery(L"overlay-brushes", brushes);
        EndPaint(hwnd_, &ps);
        return;
      }
      const auto frame = feathercast::ui::RenderTransparentFrame(dc, [&] {
        const float surfaceOpacity = static_cast<float>(
            std::clamp(overlayOpacity_.Value(), 0.0, 1.0));
        const bool opacityLayer = surfaceOpacity < 0.999f;
        if (opacityLayer) {
          dc->PushLayer(
              D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                    D2D1::IdentityMatrix(), surfaceOpacity),
              nullptr);
        }
        hits_.clear();
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const float windowScale = GetWindowScale(hwnd_);
        const float width = static_cast<float>(rc.right - rc.left) / windowScale;
        const float height = static_cast<float>(rc.bottom - rc.top) / windowScale;
        DrawObsidianBackground(
            width, height, theme_.overlayRadius,
            ObsidianBackgroundKind::Overlay);
        DrawSearch(false);
        if (confirmation_) {
          hits_.clear();
          DrawConfirmationDialog();
        }
        DrawPressedState(hwnd_);
        if (opacityLayer) dc->PopLayer();
      });
      activeRT_ = nullptr;
      activeDC_.Reset();
      if (FAILED(lastBrushFailure_)) {
        ScheduleRenderRecovery(L"overlay-brush-create", lastBrushFailure_);
      } else if (FAILED(frame.result)) {
        ScheduleRenderRecovery(L"overlay-end-draw", frame.result);
      } else {
        const HRESULT present = overlaySurface_.swapChain->Present(1, 0);
        if (FAILED(present)) {
          ScheduleRenderRecovery(L"overlay-present", present);
        }
      }
    }
    EndPaint(hwnd_, &ps);
  }

  void PaintSettings() {
    PAINTSTRUCT ps{};
    BeginPaint(settingsHwnd_, &ps);
    if (EnsureRenderResources() &&
        CreateGlassSurface(settingsSurface_, settingsHwnd_)) {
      ID2D1DeviceContext* dc = settingsSurface_.dc.Get();
      SetActiveTarget(dc);
      const HRESULT brushes = EnsureBrushResources();
      if (FAILED(brushes)) {
        activeRT_ = nullptr;
        activeDC_.Reset();
        ScheduleRenderRecovery(L"settings-brushes", brushes);
        EndPaint(settingsHwnd_, &ps);
        return;
      }
      const auto frame = feathercast::ui::RenderTransparentFrame(dc, [&] {
        const float surfaceOpacity = static_cast<float>(
            std::clamp(settingsOpacity_.Value(), 0.0, 1.0));
        const bool opacityLayer = surfaceOpacity < 0.999f;
        if (opacityLayer) {
          dc->PushLayer(
              D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                    D2D1::IdentityMatrix(), surfaceOpacity),
              nullptr);
        }
        hits_.clear();
        DrawSettings();
        DrawPressedState(settingsHwnd_);
        if (opacityLayer) dc->PopLayer();
      });
      activeRT_ = nullptr;
      activeDC_.Reset();
      if (FAILED(lastBrushFailure_)) {
        ScheduleRenderRecovery(L"settings-brush-create", lastBrushFailure_);
      } else if (FAILED(frame.result)) {
        ScheduleRenderRecovery(L"settings-end-draw", frame.result);
      } else {
        const HRESULT present = settingsSurface_.swapChain->Present(1, 0);
        if (FAILED(present)) {
          ScheduleRenderRecovery(L"settings-present", present);
        }
      }
    }
    EndPaint(settingsHwnd_, &ps);
  }

  void PaintVolumeControl() {
    PAINTSTRUCT paint{};
    BeginPaint(volumeHwnd_, &paint);
    if (EnsureRenderResources() &&
        CreateGlassSurface(volumeSurface_, volumeHwnd_)) {
      ID2D1DeviceContext* dc = volumeSurface_.dc.Get();
      SetActiveTarget(dc);
      const HRESULT brushes = EnsureBrushResources();
      if (FAILED(brushes)) {
        activeRT_ = nullptr;
        activeDC_.Reset();
        ScheduleRenderRecovery(L"volume-brushes", brushes);
        EndPaint(volumeHwnd_, &paint);
        return;
      }
      const auto frame = feathercast::ui::RenderTransparentFrame(dc, [&] {
        const float surfaceOpacity = static_cast<float>(
            std::clamp(volumeOpacity_.Value(), 0.0, 1.0));
        const bool opacityLayer = surfaceOpacity < 0.999f;
        if (opacityLayer) {
          dc->PushLayer(
              D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                    D2D1::IdentityMatrix(), surfaceOpacity),
              nullptr);
        }
        RECT client{};
        GetClientRect(volumeHwnd_, &client);
        const float scale = GetWindowScale(volumeHwnd_);
        const float width = static_cast<float>(client.right) / scale;
        const float height = static_cast<float>(client.bottom) / scale;
        DrawObsidianBackground(width, height, theme_.overlayRadius,
                               ObsidianBackgroundKind::Volume);

        DrawTextBlock(L"Volume Control", {24.0f, 18.0f, width - 150.0f, 46.0f},
                      titleFormat_.Get(), D2DColor(theme_.textPrimary));
        const double visualPercent =
            std::clamp(volumeVisualPercent_.Value(), 0.0, 100.0);
        DrawTextBlock(std::to_wstring(static_cast<int>(std::lround(visualPercent))) + L"%",
                      {width - 145.0f, 10.0f, width - 24.0f, 52.0f},
                      volumeValueFormat_.Get(), D2DColor(theme_.textPrimary));

        const RectF track = VolumeTrackRect(width);
        FillRound(track, 6.0f, D2DColor(theme_.surface));
        const float progress = static_cast<float>(visualPercent / 100.0);
        const float knobX = track.left + (track.right - track.left) * progress;
        if (knobX > track.left) {
          FillRound({track.left, track.top, knobX, track.bottom}, 6.0f,
                    D2DColor(ActiveAccent()));
        }
        auto knob = Brush(D2DColor(ActiveAccent()));
        activeRT_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(knobX, (track.top + track.bottom) / 2.0f),
                          9.0f, 9.0f),
            knob.Get());
        auto knobBorder = Brush(D2DColor(theme_.textPrimary));
        activeRT_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(knobX, (track.top + track.bottom) / 2.0f),
                          9.0f, 9.0f),
            knobBorder.Get(), 1.0f);

        const bool error = !volumeStatus_.empty();
        DrawTextBlock(error ? volumeStatus_
                            : L"Arrow keys 1%  |  Page Up/Down 10%  |  Home/End",
                      {24.0f, 122.0f, width - 24.0f, height - 18.0f},
                      bodyFormat_.Get(),
                       error ? D2DColor(theme_.danger)
                             : D2DColor(theme_.textMuted));
        if (opacityLayer) dc->PopLayer();
      });
      activeRT_ = nullptr;
      activeDC_.Reset();
      if (FAILED(lastBrushFailure_)) {
        ScheduleRenderRecovery(L"volume-brush-create", lastBrushFailure_);
      } else if (FAILED(frame.result)) {
        ScheduleRenderRecovery(L"volume-end-draw", frame.result);
      } else {
        const HRESULT present = volumeSurface_.swapChain->Present(1, 0);
        if (FAILED(present)) {
          ScheduleRenderRecovery(L"volume-present", present);
        }
      }
    }
    EndPaint(volumeHwnd_, &paint);
  }

  static uint32_t ColorKey(const D2D1_COLOR_F& color) {
    auto clamp8 = [](float v) -> uint32_t {
      const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return (clamp8(color.r) << 24) | (clamp8(color.g) << 16) | (clamp8(color.b) << 8) | clamp8(color.a);
  }

  // Reuse solid-color brushes across draw calls. Brushes are device-bound, so the
  // cache is keyed by the active render target as well as the color.
  HRESULT EnsureBrushResources() {
    lastBrushFailure_ = S_OK;
    if (!activeRT_) return E_POINTER;
    const std::array<D2D1_COLOR_F, 14> required = {
        D2DColor(theme_.overlayBackground),
        D2DColor(theme_.settingsBackground),
        D2DColor(theme_.surface),
        D2DColor(theme_.surfaceHover),
        D2DColor(theme_.selectedBase),
        D2DColor(theme_.iconTile),
        D2DColor(theme_.border),
        D2DColor(theme_.divider),
        D2DColor(theme_.textPrimary),
        D2DColor(theme_.textMuted),
        D2DColor(theme_.textDim),
        D2DColor(theme_.sectionText),
        D2DColor(ActiveAccent()),
        D2DColor(theme_.danger),
    };
    auto& byColor = brushCache_[activeRT_];
    for (const auto color : required) {
      const uint32_t key = ColorKey(color);
      if (byColor.contains(key)) continue;
      ComPtr<ID2D1SolidColorBrush> brush;
      const HRESULT result =
          activeRT_->CreateSolidColorBrush(color, brush.GetAddressOf());
      if (FAILED(result)) return result;
      byColor.emplace(key, std::move(brush));
    }
    return S_OK;
  }

  ComPtr<ID2D1SolidColorBrush> Brush(D2D1_COLOR_F color) {
    if (!activeRT_) return nullptr;
    auto& byColor = brushCache_[activeRT_];
    const uint32_t key = ColorKey(color);
    if (auto it = byColor.find(key); it != byColor.end()) return it->second;
    ComPtr<ID2D1SolidColorBrush> brush;
    const HRESULT result =
        activeRT_->CreateSolidColorBrush(color, brush.GetAddressOf());
    if (FAILED(result)) {
      lastBrushFailure_ = result;
      return byColor.empty() ? nullptr : byColor.begin()->second;
    }
    if (brush) byColor.emplace(key, brush);
    return brush;
  }

  void FillRound(RectF rect, float radius, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    activeRT_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius), brush.Get());
  }

  void FillRect(RectF rect, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    activeRT_->FillRectangle(ToD2D(rect), brush.Get());
  }

  void DrawPressedState(HWND owner) {
    if (!pointerPress_ || pointerPress_->owner != owner ||
        !pointerPress_->inside) {
      return;
    }
    const auto hit = std::find_if(hits_.begin(), hits_.end(),
                                  [&](const HitTarget& target) {
                                    return target.type == pointerPress_->type &&
                                           target.index == pointerPress_->index;
                                  });
    if (hit == hits_.end()) return;
    if (pointerPress_->type == HitType::Result) {
      if (!ResultsActivationAllowed() ||
          pointerPress_->searchGeneration != searchPresentation_.Displayed() ||
          pointerPress_->index < 0 ||
          pointerPress_->index >= static_cast<int>(flatItems_.size()) ||
          flatItems_[static_cast<size_t>(pointerPress_->index)].Key() !=
              pointerPress_->targetKey) {
        return;
      }
    }
    FillRound(hit->rect,
              pointerPress_->type == HitType::Result ? theme_.rowRadius
                                                     : theme_.controlRadius,
              D2DColor(ActiveAccent(), 0.16f));
  }

  void StrokeRound(RectF rect, float radius, D2D1_COLOR_F color, float width = 1.0f) {
    auto brush = Brush(color);
    activeRT_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius), brush.Get(), width);
  }

  enum class ObsidianBackgroundKind {
    Overlay,
    Settings,
    Volume,
  };

  void DrawObsidianBackground(float width, float height, float radius, ObsidianBackgroundKind kind) {
    const RectF panel{0.5f, 0.5f, width - 0.5f, height - 0.5f};
    const bool settings = kind == ObsidianBackgroundKind::Settings;
    const bool volume = kind == ObsidianBackgroundKind::Volume;
    const bool blurApplied = settings   ? settingsBlurApplied_
                             : volume ? volumeBlurApplied_
                                      : overlayBlurApplied_;
    feathercast::theme::Color background = settings ? theme_.settingsBackground : theme_.overlayBackground;
    background.a = std::min(background.a, blurApplied ? 0.72f : 0.85f);

    if (blurApplied) {
      // DWM owns the rounded outer silhouette while this lighter tint preserves
      // contrast without hiding the blurred desktop behind the window.
      FillRect({0.0f, 0.0f, width, height}, D2DColor(background));
    } else {
      FillRound(panel, radius, D2DColor(background));
    }
    StrokeRound(panel, radius, D2DColor(theme_.border), 1.0f);
  }

  // Set the render target for the current frame, and grab its ID2D1DeviceContext
  // interface (available on Direct2D 1.1+, i.e. all supported Windows versions)
  // so text can be drawn with color-emoji rendering enabled.
  void SetActiveTarget(ID2D1RenderTarget* rt) {
    activeRT_ = rt;
    activeDC_.Reset();
    if (rt) {
      rt->QueryInterface(activeDC_.GetAddressOf());
    }
  }

  void DrawTextBlock(const std::wstring& text, RectF rect, IDWriteTextFormat* format, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    if (activeDC_) {
      // ENABLE_COLOR_FONT makes Segoe UI Emoji glyphs render in full color, like
      // the native Windows emoji, instead of monochrome outline fallbacks.
      activeDC_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, ToD2D(rect), brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
      return;
    }
    activeRT_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, ToD2D(rect), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  ID2D1StrokeStyle* ResultIconStrokeStyle() {
    if (resultIconStroke_) return resultIconStroke_.Get();
    ComPtr<ID2D1Factory> factory;
    activeRT_->GetFactory(factory.GetAddressOf());
    if (!factory) return nullptr;
    const D2D1_STROKE_STYLE_PROPERTIES properties = {
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND,
        10.0f, D2D1_DASH_STYLE_SOLID, 0.0f};
    factory->CreateStrokeStyle(properties, nullptr, 0,
                               resultIconStroke_.GetAddressOf());
    return resultIconStroke_.Get();
  }

  void DrawResultIcon(ResultIcon icon, RectF tile, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    ID2D1StrokeStyle* stroke = ResultIconStrokeStyle();
    constexpr float sw = 1.65f;
    const float ox = tile.left + 4.0f;
    const float oy = tile.top + 4.0f;
    auto point = [&](float x, float y) { return D2D1::Point2F(ox + x, oy + y); };
    auto line = [&](float x1, float y1, float x2, float y2) {
      activeRT_->DrawLine(point(x1, y1), point(x2, y2), brush.Get(), sw, stroke);
    };
    auto rect = [&](float left, float top, float right, float bottom,
                    float radius = 1.5f) {
      activeRT_->DrawRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(ox + left, oy + top, ox + right,
                                        oy + bottom),
                            radius, radius),
          brush.Get(), sw, stroke);
    };
    auto ellipse = [&](float cx, float cy, float rx, float ry) {
      activeRT_->DrawEllipse(D2D1::Ellipse(point(cx, cy), rx, ry),
                             brush.Get(), sw, stroke);
    };
    auto dot = [&](float cx, float cy, float radius = 1.0f) {
      activeRT_->FillEllipse(D2D1::Ellipse(point(cx, cy), radius, radius),
                             brush.Get());
    };
    auto cross = [&](float cx, float cy, float radius = 2.0f) {
      line(cx - radius, cy - radius, cx + radius, cy + radius);
      line(cx + radius, cy - radius, cx - radius, cy + radius);
    };
    auto plus = [&](float cx, float cy, float radius = 2.0f) {
      line(cx - radius, cy, cx + radius, cy);
      line(cx, cy - radius, cx, cy + radius);
    };
    auto refresh = [&] {
      line(3.0f, 6.0f, 4.2f, 3.8f);
      line(4.2f, 3.8f, 6.5f, 3.2f);
      line(4.2f, 3.8f, 4.4f, 6.3f);
      line(6.5f, 3.2f, 10.8f, 4.2f);
      line(10.8f, 4.2f, 12.5f, 7.0f);
      line(13.0f, 10.0f, 11.8f, 12.2f);
      line(11.8f, 12.2f, 9.5f, 12.8f);
      line(11.8f, 12.2f, 11.6f, 9.7f);
      line(9.5f, 12.8f, 5.2f, 11.8f);
      line(5.2f, 11.8f, 3.5f, 9.0f);
    };
    auto document = [&] {
      line(3.5f, 1.8f, 9.8f, 1.8f);
      line(9.8f, 1.8f, 12.7f, 4.7f);
      line(12.7f, 4.7f, 12.7f, 14.2f);
      line(12.7f, 14.2f, 3.5f, 14.2f);
      line(3.5f, 14.2f, 3.5f, 1.8f);
      line(9.8f, 1.8f, 9.8f, 4.7f);
      line(9.8f, 4.7f, 12.7f, 4.7f);
      line(5.7f, 7.5f, 10.5f, 7.5f);
      line(5.7f, 10.3f, 9.5f, 10.3f);
    };
    auto clipboard = [&] {
      rect(3.0f, 3.0f, 13.0f, 14.5f, 1.7f);
      rect(5.5f, 1.5f, 10.5f, 4.5f, 1.2f);
      line(5.5f, 7.2f, 10.5f, 7.2f);
      line(5.5f, 10.2f, 9.5f, 10.2f);
    };
    auto folder = [&] {
      line(1.7f, 4.2f, 6.0f, 4.2f);
      line(6.0f, 4.2f, 7.5f, 6.0f);
      line(7.5f, 6.0f, 14.3f, 6.0f);
      line(14.3f, 6.0f, 13.2f, 13.3f);
      line(13.2f, 13.3f, 2.8f, 13.3f);
      line(2.8f, 13.3f, 1.7f, 4.2f);
    };
    auto monitor = [&] {
      rect(1.7f, 2.5f, 14.3f, 11.5f, 1.5f);
      line(8.0f, 11.5f, 8.0f, 14.0f);
      line(5.3f, 14.0f, 10.7f, 14.0f);
    };
    auto speaker = [&] {
      line(1.7f, 6.2f, 4.7f, 6.2f);
      line(4.7f, 6.2f, 8.3f, 3.4f);
      line(8.3f, 3.4f, 8.3f, 12.6f);
      line(8.3f, 12.6f, 4.7f, 9.8f);
      line(4.7f, 9.8f, 1.7f, 9.8f);
      line(1.7f, 9.8f, 1.7f, 6.2f);
      line(10.5f, 6.0f, 11.5f, 7.0f);
      line(11.5f, 7.0f, 11.5f, 9.0f);
      line(11.5f, 9.0f, 10.5f, 10.0f);
    };
    auto windowFrame = [&] {
      rect(2.0f, 2.5f, 14.0f, 13.5f, 1.5f);
      line(2.0f, 5.3f, 14.0f, 5.3f);
      dot(4.0f, 3.9f, 0.65f);
    };
    auto pane = [&](ResultIcon paneIcon) {
      rect(2.0f, 2.0f, 14.0f, 14.0f, 1.5f);
      switch (paneIcon) {
        case ResultIcon::WindowLeft:
          line(8.0f, 2.0f, 8.0f, 14.0f);
          line(6.5f, 8.0f, 4.2f, 8.0f);
          line(4.2f, 8.0f, 5.5f, 6.7f);
          line(4.2f, 8.0f, 5.5f, 9.3f);
          break;
        case ResultIcon::WindowRight:
          line(8.0f, 2.0f, 8.0f, 14.0f);
          line(9.5f, 8.0f, 11.8f, 8.0f);
          line(11.8f, 8.0f, 10.5f, 6.7f);
          line(11.8f, 8.0f, 10.5f, 9.3f);
          break;
        case ResultIcon::WindowTop:
          line(2.0f, 8.0f, 14.0f, 8.0f);
          line(8.0f, 6.5f, 8.0f, 4.2f);
          line(8.0f, 4.2f, 6.7f, 5.5f);
          line(8.0f, 4.2f, 9.3f, 5.5f);
          break;
        case ResultIcon::WindowBottom:
          line(2.0f, 8.0f, 14.0f, 8.0f);
          line(8.0f, 9.5f, 8.0f, 11.8f);
          line(8.0f, 11.8f, 6.7f, 10.5f);
          line(8.0f, 11.8f, 9.3f, 10.5f);
          break;
        default: break;
      }
    };

    switch (icon) {
      case ResultIcon::App:
        windowFrame();
        plus(8.0f, 9.2f, 2.0f);
        break;
      case ResultIcon::AppGrid:
        rect(2.0f, 2.0f, 6.5f, 6.5f, 1.0f);
        rect(9.5f, 2.0f, 14.0f, 6.5f, 1.0f);
        rect(2.0f, 9.5f, 6.5f, 14.0f, 1.0f);
        rect(9.5f, 9.5f, 14.0f, 14.0f, 1.0f);
        break;
      case ResultIcon::Gamepad:
        line(3.0f, 6.0f, 5.0f, 4.0f);
        line(5.0f, 4.0f, 11.0f, 4.0f);
        line(11.0f, 4.0f, 13.0f, 6.0f);
        line(13.0f, 6.0f, 14.2f, 11.2f);
        line(14.2f, 11.2f, 12.8f, 13.0f);
        line(12.8f, 13.0f, 9.8f, 10.5f);
        line(9.8f, 10.5f, 6.2f, 10.5f);
        line(6.2f, 10.5f, 3.2f, 13.0f);
        line(3.2f, 13.0f, 1.8f, 11.2f);
        line(1.8f, 11.2f, 3.0f, 6.0f);
        plus(5.3f, 7.5f, 1.5f);
        dot(10.7f, 7.0f, 0.8f);
        dot(12.2f, 8.5f, 0.8f);
        break;
      case ResultIcon::Windows:
        rect(1.8f, 4.5f, 11.5f, 13.8f, 1.3f);
        rect(4.5f, 1.8f, 14.2f, 11.1f, 1.3f);
        line(4.5f, 4.7f, 14.2f, 4.7f);
        break;
      case ResultIcon::WindowLayout:
        rect(1.8f, 2.0f, 14.2f, 14.0f, 1.4f);
        line(7.2f, 2.0f, 7.2f, 14.0f);
        line(7.2f, 8.0f, 14.2f, 8.0f);
        break;
      case ResultIcon::Actions:
        for (int row = 0; row < 3; ++row) {
          const float y = 4.0f + row * 4.0f;
          dot(3.0f, y, 0.9f);
          line(6.0f, y, 13.0f, y);
        }
        break;
      case ResultIcon::Calculator:
        rect(2.0f, 1.5f, 14.0f, 14.5f, 1.7f);
        line(4.0f, 5.0f, 12.0f, 5.0f);
        plus(5.0f, 9.0f, 1.4f);
        line(9.5f, 9.0f, 12.0f, 9.0f);
        cross(5.0f, 12.2f, 1.2f);
        line(9.5f, 11.5f, 12.0f, 11.5f);
        line(9.5f, 13.0f, 12.0f, 13.0f);
        break;
      case ResultIcon::Convert:
        line(2.0f, 5.0f, 12.5f, 5.0f);
        line(12.5f, 5.0f, 10.3f, 2.8f);
        line(12.5f, 5.0f, 10.3f, 7.2f);
        line(14.0f, 11.0f, 3.5f, 11.0f);
        line(3.5f, 11.0f, 5.7f, 8.8f);
        line(3.5f, 11.0f, 5.7f, 13.2f);
        break;
      case ResultIcon::Clock:
        ellipse(8.0f, 8.0f, 6.2f, 6.2f);
        line(8.0f, 4.5f, 8.0f, 8.0f);
        line(8.0f, 8.0f, 10.6f, 9.5f);
        break;
      case ResultIcon::Calendar:
      case ResultIcon::CalendarWeek:
        rect(2.0f, 3.0f, 14.0f, 14.0f, 1.5f);
        line(2.0f, 6.2f, 14.0f, 6.2f);
        line(5.0f, 1.8f, 5.0f, 4.2f);
        line(11.0f, 1.8f, 11.0f, 4.2f);
        if (icon == ResultIcon::CalendarWeek) {
          line(5.0f, 9.0f, 11.0f, 9.0f);
          line(5.0f, 11.7f, 9.0f, 11.7f);
        } else {
          dot(5.2f, 9.2f, 0.75f);
          dot(8.0f, 9.2f, 0.75f);
          dot(10.8f, 9.2f, 0.75f);
        }
        break;
      case ResultIcon::Code:
        line(6.0f, 3.2f, 2.0f, 8.0f);
        line(2.0f, 8.0f, 6.0f, 12.8f);
        line(10.0f, 3.2f, 14.0f, 8.0f);
        line(14.0f, 8.0f, 10.0f, 12.8f);
        line(9.0f, 2.5f, 7.0f, 13.5f);
        break;
      case ResultIcon::WebSearch:
        ellipse(6.8f, 6.8f, 5.0f, 5.0f);
        ellipse(6.8f, 6.8f, 2.2f, 5.0f);
        line(2.2f, 6.8f, 11.4f, 6.8f);
        line(10.4f, 10.4f, 14.0f, 14.0f);
        break;
      case ResultIcon::Smile:
        ellipse(8.0f, 8.0f, 6.2f, 6.2f);
        dot(5.6f, 6.3f, 0.8f);
        dot(10.4f, 6.3f, 0.8f);
        line(5.2f, 10.0f, 7.0f, 11.4f);
        line(7.0f, 11.4f, 9.0f, 11.4f);
        line(9.0f, 11.4f, 10.8f, 10.0f);
        break;
      case ResultIcon::Symbols:
        plus(4.0f, 4.0f, 2.0f);
        ellipse(11.7f, 4.0f, 2.0f, 2.0f);
        line(2.0f, 12.0f, 5.0f, 8.5f);
        line(5.0f, 8.5f, 8.0f, 12.0f);
        line(8.0f, 12.0f, 5.0f, 14.0f);
        line(5.0f, 14.0f, 2.0f, 12.0f);
        line(10.0f, 10.0f, 14.0f, 14.0f);
        line(14.0f, 10.0f, 10.0f, 14.0f);
        break;
      case ResultIcon::FolderSearch:
        folder();
        ellipse(10.8f, 10.5f, 2.2f, 2.2f);
        line(12.3f, 12.0f, 14.4f, 14.1f);
        break;
      case ResultIcon::Clipboard:
        clipboard();
        break;
      case ResultIcon::ClipboardOff:
        clipboard();
        line(2.0f, 2.0f, 14.0f, 14.0f);
        break;
      case ResultIcon::Document:
        document();
        break;
      case ResultIcon::DocumentRefresh:
        document();
        refresh();
        break;
      case ResultIcon::Link:
        ellipse(5.2f, 9.8f, 3.8f, 2.7f);
        ellipse(10.8f, 6.2f, 3.8f, 2.7f);
        line(5.8f, 9.5f, 10.2f, 6.5f);
        break;
      case ResultIcon::Terminal:
        rect(1.5f, 2.0f, 14.5f, 14.0f, 1.8f);
        line(1.5f, 5.0f, 14.5f, 5.0f);
        line(4.0f, 8.0f, 6.0f, 10.0f);
        line(6.0f, 10.0f, 4.0f, 12.0f);
        line(8.0f, 12.0f, 11.5f, 12.0f);
        break;
      case ResultIcon::Gear:
        ellipse(8.0f, 8.0f, 4.3f, 4.3f);
        ellipse(8.0f, 8.0f, 1.5f, 1.5f);
        for (int i = 0; i < 8; ++i) {
          const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
          line(8.0f + std::cos(angle) * 4.5f, 8.0f + std::sin(angle) * 4.5f,
               8.0f + std::cos(angle) * 6.4f, 8.0f + std::sin(angle) * 6.4f);
        }
        break;
      case ResultIcon::Puzzle:
      case ResultIcon::PuzzleRefresh:
        line(3.0f, 3.0f, 6.0f, 3.0f);
        ellipse(8.0f, 3.0f, 2.0f, 2.0f);
        line(10.0f, 3.0f, 13.0f, 3.0f);
        line(13.0f, 3.0f, 13.0f, 7.0f);
        ellipse(13.0f, 9.0f, 2.0f, 2.0f);
        line(13.0f, 11.0f, 13.0f, 13.0f);
        line(13.0f, 13.0f, 3.0f, 13.0f);
        line(3.0f, 13.0f, 3.0f, 3.0f);
        if (icon == ResultIcon::PuzzleRefresh) refresh();
        break;
      case ResultIcon::Keyboard:
        rect(1.5f, 3.0f, 14.5f, 13.0f, 1.7f);
        for (int row = 0; row < 2; ++row) {
          for (int col = 0; col < 4; ++col) {
            dot(4.0f + col * 2.7f, 6.0f + row * 2.7f, 0.65f);
          }
        }
        line(5.0f, 11.0f, 11.0f, 11.0f);
        break;
      case ResultIcon::Compass:
        ellipse(8.0f, 8.0f, 6.2f, 6.2f);
        line(10.7f, 5.3f, 9.2f, 9.2f);
        line(9.2f, 9.2f, 5.3f, 10.7f);
        line(5.3f, 10.7f, 6.8f, 6.8f);
        line(6.8f, 6.8f, 10.7f, 5.3f);
        break;
      case ResultIcon::Exit:
        line(8.0f, 2.0f, 3.0f, 2.0f);
        line(3.0f, 2.0f, 3.0f, 14.0f);
        line(3.0f, 14.0f, 8.0f, 14.0f);
        line(6.0f, 8.0f, 14.0f, 8.0f);
        line(14.0f, 8.0f, 11.5f, 5.5f);
        line(14.0f, 8.0f, 11.5f, 10.5f);
        break;
      case ResultIcon::Refresh:
        refresh();
        break;
      case ResultIcon::Trash:
        rect(3.5f, 5.0f, 12.5f, 14.0f, 1.2f);
        line(2.0f, 4.0f, 14.0f, 4.0f);
        line(6.0f, 2.0f, 10.0f, 2.0f);
        line(6.3f, 7.0f, 6.3f, 11.8f);
        line(9.7f, 7.0f, 9.7f, 11.8f);
        break;
      case ResultIcon::HistoryOff:
        ellipse(8.0f, 8.0f, 6.0f, 6.0f);
        line(8.0f, 4.5f, 8.0f, 8.0f);
        line(8.0f, 8.0f, 5.5f, 9.2f);
        line(2.0f, 2.0f, 14.0f, 14.0f);
        break;
      case ResultIcon::Folder:
        folder();
        break;
      case ResultIcon::FolderGear:
        folder();
        ellipse(10.8f, 10.7f, 2.2f, 2.2f);
        dot(10.8f, 10.7f, 0.7f);
        break;
      case ResultIcon::File:
        document();
        break;
      case ResultIcon::Database:
        ellipse(8.0f, 4.0f, 5.5f, 2.2f);
        line(2.5f, 4.0f, 2.5f, 12.0f);
        line(13.5f, 4.0f, 13.5f, 12.0f);
        line(2.5f, 8.0f, 4.0f, 9.3f);
        line(4.0f, 9.3f, 8.0f, 10.0f);
        line(8.0f, 10.0f, 12.0f, 9.3f);
        line(12.0f, 9.3f, 13.5f, 8.0f);
        ellipse(8.0f, 12.0f, 5.5f, 2.2f);
        break;
      case ResultIcon::Download:
        line(8.0f, 2.0f, 8.0f, 10.0f);
        line(8.0f, 10.0f, 5.0f, 7.0f);
        line(8.0f, 10.0f, 11.0f, 7.0f);
        line(3.0f, 13.5f, 13.0f, 13.5f);
        break;
      case ResultIcon::Lock:
        rect(3.0f, 7.0f, 13.0f, 14.0f, 1.5f);
        line(5.0f, 7.0f, 5.0f, 5.2f);
        line(5.0f, 5.2f, 6.0f, 3.0f);
        line(6.0f, 3.0f, 10.0f, 3.0f);
        line(10.0f, 3.0f, 11.0f, 5.2f);
        line(11.0f, 5.2f, 11.0f, 7.0f);
        dot(8.0f, 10.5f, 1.0f);
        break;
      case ResultIcon::Moon:
        ellipse(7.5f, 8.0f, 5.7f, 6.0f);
        line(9.0f, 2.5f, 11.0f, 5.0f);
        line(11.0f, 5.0f, 11.5f, 8.5f);
        line(11.5f, 8.5f, 10.0f, 11.3f);
        break;
      case ResultIcon::Power:
        line(8.0f, 1.5f, 8.0f, 7.5f);
        line(4.5f, 4.0f, 2.7f, 6.5f);
        line(2.7f, 6.5f, 2.7f, 10.0f);
        line(2.7f, 10.0f, 5.0f, 13.2f);
        line(5.0f, 13.2f, 11.0f, 13.2f);
        line(11.0f, 13.2f, 13.3f, 10.0f);
        line(13.3f, 10.0f, 13.3f, 6.5f);
        line(13.3f, 6.5f, 11.5f, 4.0f);
        break;
      case ResultIcon::PowerRefresh:
        line(8.0f, 1.5f, 8.0f, 7.0f);
        refresh();
        break;
      case ResultIcon::Speaker:
      case ResultIcon::SpeakerOff:
      case ResultIcon::SpeakerPlus:
      case ResultIcon::SpeakerMinus:
        speaker();
        if (icon == ResultIcon::SpeakerOff) cross(13.2f, 8.0f, 2.0f);
        if (icon == ResultIcon::SpeakerPlus) plus(13.0f, 8.0f, 2.0f);
        if (icon == ResultIcon::SpeakerMinus) line(11.0f, 8.0f, 15.0f, 8.0f);
        break;
      case ResultIcon::Sliders:
        line(3.0f, 2.0f, 3.0f, 14.0f);
        line(8.0f, 2.0f, 8.0f, 14.0f);
        line(13.0f, 2.0f, 13.0f, 14.0f);
        ellipse(3.0f, 6.0f, 1.5f, 1.5f);
        ellipse(8.0f, 10.0f, 1.5f, 1.5f);
        ellipse(13.0f, 5.0f, 1.5f, 1.5f);
        break;
      case ResultIcon::PlayPause:
        line(2.5f, 3.0f, 2.5f, 13.0f);
        line(2.5f, 3.0f, 9.0f, 8.0f);
        line(9.0f, 8.0f, 2.5f, 13.0f);
        line(11.5f, 3.0f, 11.5f, 13.0f);
        line(14.0f, 3.0f, 14.0f, 13.0f);
        break;
      case ResultIcon::NextTrack:
      case ResultIcon::PreviousTrack: {
        const bool next = icon == ResultIcon::NextTrack;
        const float left = next ? 2.0f : 14.0f;
        const float tip = next ? 11.0f : 5.0f;
        line(left, 3.0f, tip, 8.0f);
        line(tip, 8.0f, left, 13.0f);
        line(next ? 14.0f : 2.0f, 3.0f, next ? 14.0f : 2.0f, 13.0f);
        break;
      }
      case ResultIcon::Monitor:
        monitor();
        break;
      case ResultIcon::Palette:
      case ResultIcon::PaletteRefresh:
        ellipse(7.5f, 8.0f, 6.0f, 5.8f);
        dot(5.0f, 5.3f, 0.9f);
        dot(8.0f, 4.2f, 0.9f);
        dot(10.8f, 6.0f, 0.9f);
        ellipse(9.8f, 10.8f, 2.2f, 1.6f);
        if (icon == ResultIcon::PaletteRefresh) refresh();
        break;
      case ResultIcon::ExternalLink:
        rect(2.0f, 4.5f, 11.5f, 14.0f, 1.4f);
        line(7.0f, 9.0f, 14.0f, 2.0f);
        line(9.5f, 2.0f, 14.0f, 2.0f);
        line(14.0f, 2.0f, 14.0f, 6.5f);
        break;
      case ResultIcon::Shield:
        line(8.0f, 1.5f, 13.0f, 3.5f);
        line(13.0f, 3.5f, 12.2f, 10.5f);
        line(12.2f, 10.5f, 8.0f, 14.5f);
        line(8.0f, 14.5f, 3.8f, 10.5f);
        line(3.8f, 10.5f, 3.0f, 3.5f);
        line(3.0f, 3.5f, 8.0f, 1.5f);
        line(8.0f, 3.5f, 8.0f, 12.0f);
        break;
      case ResultIcon::Copy:
        rect(2.0f, 4.5f, 11.5f, 14.0f, 1.4f);
        rect(4.5f, 2.0f, 14.0f, 11.5f, 1.4f);
        break;
      case ResultIcon::Pin:
      case ResultIcon::PinOff:
        line(5.0f, 2.0f, 11.0f, 2.0f);
        line(6.0f, 2.0f, 6.0f, 6.0f);
        line(10.0f, 2.0f, 10.0f, 6.0f);
        line(4.0f, 8.0f, 12.0f, 8.0f);
        line(6.0f, 6.0f, 4.0f, 8.0f);
        line(10.0f, 6.0f, 12.0f, 8.0f);
        line(8.0f, 8.0f, 8.0f, 14.0f);
        if (icon == ResultIcon::PinOff) line(2.0f, 2.0f, 14.0f, 14.0f);
        break;
      case ResultIcon::Eye:
      case ResultIcon::EyeOff:
        line(1.5f, 8.0f, 4.5f, 4.7f);
        line(4.5f, 4.7f, 8.0f, 3.5f);
        line(8.0f, 3.5f, 11.5f, 4.7f);
        line(11.5f, 4.7f, 14.5f, 8.0f);
        line(14.5f, 8.0f, 11.5f, 11.3f);
        line(11.5f, 11.3f, 8.0f, 12.5f);
        line(8.0f, 12.5f, 4.5f, 11.3f);
        line(4.5f, 11.3f, 1.5f, 8.0f);
        ellipse(8.0f, 8.0f, 2.0f, 2.0f);
        if (icon == ResultIcon::EyeOff) line(2.0f, 2.0f, 14.0f, 14.0f);
        break;
      case ResultIcon::Minimize:
        line(3.0f, 12.0f, 13.0f, 12.0f);
        break;
      case ResultIcon::Maximize:
        rect(2.0f, 4.5f, 11.5f, 14.0f, 1.2f);
        rect(4.5f, 2.0f, 14.0f, 11.5f, 1.2f);
        break;
      case ResultIcon::Close:
        cross(8.0f, 8.0f, 5.0f);
        break;
      case ResultIcon::WindowLeft:
      case ResultIcon::WindowRight:
      case ResultIcon::WindowTop:
      case ResultIcon::WindowBottom:
        pane(icon);
        break;
      case ResultIcon::Center:
        rect(2.0f, 2.0f, 14.0f, 14.0f, 1.5f);
        rect(5.0f, 5.0f, 11.0f, 11.0f, 1.0f);
        break;
      case ResultIcon::MultiMonitor:
        rect(1.0f, 2.0f, 10.5f, 9.5f, 1.2f);
        rect(5.5f, 6.5f, 15.0f, 14.0f, 1.2f);
        line(9.0f, 4.0f, 12.0f, 4.0f);
        line(12.0f, 4.0f, 10.5f, 2.5f);
        line(12.0f, 4.0f, 10.5f, 5.5f);
        break;
      case ResultIcon::Edit:
        line(3.0f, 13.0f, 5.2f, 8.2f);
        line(5.2f, 8.2f, 11.8f, 1.6f);
        line(11.8f, 1.6f, 14.4f, 4.2f);
        line(14.4f, 4.2f, 7.8f, 10.8f);
        line(7.8f, 10.8f, 3.0f, 13.0f);
        break;
    }
  }

  void DrawSearchIcon(float x, float y, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    activeRT_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 8, y + 8), 6, 6), brush.Get(), 2.0f);
    activeRT_->DrawLine(D2D1::Point2F(x + 13, y + 13), D2D1::Point2F(x + 19, y + 19), brush.Get(), 2.0f);
  }

  void DrawGearIcon(float cx, float cy, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    activeRT_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5.0f, 5.0f), brush.Get(), 2.0f);
    activeRT_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.5f, 1.5f), brush.Get(), 1.5f);
    for (int i = 0; i < 8; ++i) {
      float angle = i * 3.14159265f / 4.0f;
      float sinA = std::sin(angle);
      float cosA = std::cos(angle);
      activeRT_->DrawLine(
        D2D1::Point2F(cx + cosA * 5.0f, cy + sinA * 5.0f),
        D2D1::Point2F(cx + cosA * 8.0f, cy + sinA * 8.0f),
        brush.Get(),
        2.0f
      );
    }
  }

  static bool CaretPhase() {
    const UINT blink = GetCaretBlinkTime();
    return (GetTickCount() / (blink ? blink : 530)) % 2 == 0;
  }

  // Width of `text` in the input font, used to place the caret. Cached so a steady
  // (non-typing) overlay doesn't rebuild an IDWriteTextLayout on every repaint.
  float MeasureCaretOffset(const std::wstring& text, float width) {
    if (text == caretMeasureText_ && width == caretMeasureWidth_) return caretOffset_;
    float offset = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                   inputFormat_.Get(), width - 94 - 52, 48,
                                                   layout.GetAddressOf()))) {
      DWRITE_TEXT_METRICS metrics{};
      layout->GetMetrics(&metrics);
      offset = metrics.width;
    }
    caretMeasureText_ = text;
    caretMeasureWidth_ = width;
    caretOffset_ = offset;
    return offset;
  }

  void DrawPreviewPane(RectF rect) {
    FillRound(rect, 8.0f, D2DColor(theme_.surface));
    StrokeRound(rect, 8.0f, D2DColor(theme_.border));
    const float left = rect.left + 18.0f;
    const float right = rect.right - 18.0f;
    if (!previewResult_) {
      DrawTextBlock(L"Loading preview...",
                    {left, rect.top + 22.0f, right, rect.top + 52.0f},
                    bodyFormat_.Get(), D2DColor(theme_.textMuted));
      return;
    }
    const auto& preview = *previewResult_;
    DrawTextBlock(preview.title,
                  {left, rect.top + 16.0f, right, rect.top + 44.0f},
                  titleFormat_.Get(), D2DColor(theme_.textPrimary));
    DrawTextBlock(preview.detail,
                  {left, rect.top + 48.0f, right, rect.top + 112.0f},
                  subFormat_.Get(),
                  preview.kind == feathercast::preview::Kind::Error
                      ? D2DColor(theme_.danger)
                      : D2DColor(theme_.textMuted));
    const float contentTop = rect.top + 120.0f;
    if (preview.kind == feathercast::preview::Kind::Text) {
      activeRT_->PushAxisAlignedClip(
          D2D1::RectF(left, contentTop, right, rect.bottom - 16.0f),
          D2D1_ANTIALIAS_MODE_ALIASED);
      DrawTextBlock(preview.text,
                    {left, contentTop - previewScroll_, right,
                     contentTop - previewScroll_ + 4096.0f},
                    bodyFormat_.Get(), D2DColor(theme_.textPrimary));
      activeRT_->PopAxisAlignedClip();
    } else if (preview.kind == feathercast::preview::Kind::Image &&
               !preview.pixels.empty()) {
      if (!previewBitmap_) {
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        activeRT_->CreateBitmap(
            D2D1::SizeU(preview.width, preview.height), preview.pixels.data(),
            preview.stride, properties, previewBitmap_.GetAddressOf());
      }
      if (previewBitmap_) {
        const float availableWidth = right - left;
        const float availableHeight = rect.bottom - contentTop - 16.0f;
        const float scale = std::min(
            availableWidth / static_cast<float>(preview.width),
            availableHeight / static_cast<float>(preview.height));
        const float drawWidth = preview.width * scale;
        const float drawHeight = preview.height * scale;
        const float x = left + (availableWidth - drawWidth) / 2.0f;
        const float y = contentTop + (availableHeight - drawHeight) / 2.0f;
        activeRT_->DrawBitmap(
            previewBitmap_.Get(),
            D2D1::RectF(x, y, x + drawWidth, y + drawHeight), 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      }
    }
  }

  void DrawSearch(bool drawBackground) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetWindowScale(hwnd_);
    const float width = static_cast<float>(rc.right - rc.left) / scale;
    const float height = static_cast<float>(rc.bottom - rc.top) / scale;
    const COLORREF accent = ActiveAccent();

    if (drawBackground) {
      DrawObsidianBackground(
          width, height, theme_.overlayRadius, ObsidianBackgroundKind::Overlay);
    }

    DrawSearchIcon(18, 20, D2DColor(theme_.textMuted));
    std::wstring displayedQuery = query_;
    if (!imeComposition_.empty()) {
      displayedQuery.insert(std::min(caret_, displayedQuery.size()), imeComposition_);
    }
    const std::wstring input = displayedQuery.empty() ? SearchPlaceholder() : displayedQuery;
    if (const auto range = SelectionRange()) {
      const float selectionLeft = 52.0f + MeasureCaretOffset(query_.substr(0, range->first), width);
      const float selectionRight = 52.0f + MeasureCaretOffset(query_.substr(0, range->second), width);
      FillRound({selectionLeft, 17.0f, std::max(selectionLeft + 1.0f, selectionRight), 46.0f},
                3.0f, Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.35f, 0.85f));
    }
    DrawTextBlock(input, {52, 15, width - 94, 48}, inputFormat_.Get(), displayedQuery.empty() ? D2DColor(theme_.textMuted) : D2DColor(theme_.textPrimary));

    float caretX = 52.0f;
    if (!displayedQuery.empty()) {
      ClampCaret();
      std::wstring caretPrefix = query_.substr(0, caret_);
      caretPrefix += imeComposition_;
      caretX = 52.0f + MeasureCaretOffset(caretPrefix, width);
    }
    if (!imeComposition_.empty()) {
      const float compositionLeft = 52.0f + MeasureCaretOffset(query_.substr(0, caret_), width);
      auto underline = Brush(D2DColor(accent));
      activeRT_->DrawLine(D2D1::Point2F(compositionLeft, 44.0f),
                          D2D1::Point2F(caretX, 44.0f), underline.Get(), 1.0f);
    }
    const bool showCaret = CaretPhase();
    if (showCaret) {
      auto caretBrush = Brush(D2DColor(accent));
      activeRT_->DrawLine(
          D2D1::Point2F(caretX, 20.0f),
          D2D1::Point2F(caretX, 42.0f),
          caretBrush.Get(),
          1.5f
      );
    }

    if (caching_) {
      FillRound({width - 82, 26, width - 74, 34}, 4, D2DColor(accent));
    }
    hits_.push_back({{width - 52, 14, width - 16, 50}, HitType::Gear});
    if (gearHovered_) {
      FillRound({width - 50, 16, width - 18, 48}, theme_.controlRadius, D2DColor(theme_.surfaceHover));
    }
    DrawGearIcon(width - 34, 32, gearHovered_ ? D2DColor(theme_.textPrimary) : D2DColor(theme_.textMuted));

    if (!settings_.compactMode || !query_.empty()) {
      auto border = Brush(D2DColor(theme_.border));
      activeRT_->DrawLine(D2D1::Point2F(0, 60), D2D1::Point2F(width, 60), border.Get(), 1);
    }

    if (settings_.compactMode && query_.empty() && !actionMode_ && browseView_ == BrowseView::None) return;

    const bool showFooter = !settings_.compactMode;
    const float footerHeight = showFooter ? 40.0f : 0.0f;
    const float resultsTop = kResultsTop;
    const float resultsBottom = height - footerHeight;
    const int baseOverlayWidth = std::clamp(settings_.overlayWidth,
                                            MIN_OVERLAY_WIDTH,
                                            MAX_OVERLAY_WIDTH);
    const bool previewSideBySide =
        previewOpen_ && width >= static_cast<float>(baseOverlayWidth + 340);
    const DisplayItem* detailItem = previewOpen_ ? nullptr : SelectedMarkdownDetailItem();
    const float detailWidth = previewOpen_
                                  ? (previewSideBySide ? 420.0f : width - 16.0f)
                                  : (detailItem ? MarkdownDetailWidth(width) : 0.0f);
    if (detailWidth <= 0.0f) detailItem = nullptr;
    const float resultsRight = previewOpen_
                                   ? (previewSideBySide
                                          ? width - detailWidth - 8.0f
                                          : width)
                                   : (detailItem ? width - detailWidth - 8.0f
                                                 : width);
    const float viewHeight = resultsBottom - resultsTop;
    const int contentHeight = ResultsContentHeight();
    feathercast::ui::OverlayController::SetScroll(
        overlayState_, scroll_, contentHeight - static_cast<int>(viewHeight));
    if (std::abs(overlayVisualScroll_.Target() -
                 static_cast<double>(scroll_)) > 0.001) {
      RetargetOverlayScroll();
    }

    float y = resultsTop -
              static_cast<float>(overlayVisualScroll_.Value());
    int rowIndex = 0;
    activeRT_->PushAxisAlignedClip(D2D1::RectF(0, resultsTop, resultsRight, resultsBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    DrawSelectionPill(resultsRight);
    for (const auto& section : sections_) {
      float headerY = y;
      float headerOpacity = 1.0f;
      if (const auto motion = resultHeaderMotion_.find(section.title);
          motion != resultHeaderMotion_.end()) {
        headerY += static_cast<float>(motion->second.offsetY.Value());
        headerOpacity = static_cast<float>(motion->second.opacity.Value());
      }
      if (headerY + kSectionHeaderHeight >= resultsTop &&
          headerY <= resultsBottom) {
        const bool headerLayer = headerOpacity < 0.999f;
        if (headerLayer) {
          activeRT_->PushLayer(
              D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                    D2D1::IdentityMatrix(), headerOpacity),
              nullptr);
        }
        DrawTextBlock(section.title,
                      {12, headerY + 8, resultsRight - 12, headerY + 24},
                      sectionFormat_.Get(), D2DColor(theme_.sectionText));
        if (headerLayer) activeRT_->PopLayer();
      }
      y += kSectionHeaderHeight;
      for (const auto& item : section.items) {
        // 50px row height (was 46px): subtitle no longer clips at the bottom edge.
        float rowOffset = 0.0f;
        float rowOpacity = 1.0f;
        if (const auto motion = resultRowMotion_.find(item.Key());
            motion != resultRowMotion_.end()) {
          rowOffset = static_cast<float>(motion->second.offsetY.Value());
          rowOpacity = static_cast<float>(motion->second.opacity.Value());
        }
        RectF rowRect{8, y + rowOffset, resultsRight - 8,
                      y + rowOffset + kResultRowHeight};
        if (rowRect.bottom >= resultsTop && rowRect.top <= resultsBottom) {
          const RowAnim anim = ComputeRowAnim(rowIndex);
          const float combinedOpacity = anim.opacity * rowOpacity;
          if (combinedOpacity < 0.999f || anim.dy != 0.0f) {
            D2D1_MATRIX_3X2_F base;
            activeRT_->GetTransform(&base);
            activeRT_->SetTransform(
                D2D1::Matrix3x2F::Translation(0.0f, anim.dy) * base);
            activeRT_->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(),
                                      combinedOpacity),
                nullptr);
            DrawResultRow(item, rowRect, rowIndex);
            activeRT_->PopLayer();
            activeRT_->SetTransform(base);
          } else {
            DrawResultRow(item, rowRect, rowIndex);
          }
        }
        // 52px stride = 50px row + 2px gap (was 48 = 46 + 2).
        y += kResultRowStride;
        ++rowIndex;
      }
    }
    activeRT_->PopAxisAlignedClip();

    if (detailItem) {
      DrawMarkdownDetail(*detailItem, {resultsRight + 4.0f, resultsTop + 8.0f, width - 8.0f, resultsBottom - 8.0f});
    }
    if (previewOpen_) {
      DrawPreviewPane(previewSideBySide
                          ? RectF{resultsRight + 4.0f, resultsTop + 8.0f,
                                  width - 8.0f, resultsBottom - 8.0f}
                          : RectF{8.0f, resultsTop + 8.0f, width - 8.0f,
                                  resultsBottom - 8.0f});
      if (!previewSideBySide) hits_.clear();
    }

    if (sections_.empty()) {
      DrawTextBlock(EmptyResultsMessage(),
                    {0, 170, width, 210}, centerFormat_.Get(), D2DColor(theme_.textMuted));
    } else if (SearchPending()) {
      const RectF statusRect{std::max(16.0f, resultsRight - 132.0f), 68.0f,
                             resultsRight - 16.0f, 94.0f};
      D2D1_COLOR_F statusColor = D2DColor(theme_.surface);
      statusColor.a = 0.96f;
      FillRound(statusRect, 13.0f, statusColor);
      StrokeRound(statusRect, 13.0f, D2DColor(theme_.border));
      DrawTextBlock(L"Searching...", statusRect, centerFormat_.Get(),
                    D2DColor(theme_.textMuted));
    }

    if (!showFooter && overlayStatus_) {
      const RectF statusRect{16.0f, height - 36.0f, width - 16.0f,
                             height - 8.0f};
      D2D1_COLOR_F background = D2DColor(theme_.surface);
      background.a = 0.96f;
      FillRound(statusRect, 12.0f, background);
      StrokeRound(statusRect, 12.0f, D2DColor(theme_.border));
      DrawTextBlock(
          overlayStatus_->text, statusRect, centerFormat_.Get(),
          overlayStatus_->severity == StatusSeverity::Error
              ? D2DColor(theme_.danger)
              : D2DColor(theme_.textMuted));
    }

    if (showFooter) {
      auto border = Brush(D2DColor(theme_.divider));
      activeRT_->DrawLine(D2D1::Point2F(0, height - 40.0f), D2D1::Point2F(width, height - 40.0f), border.Get(), 1.0f);
      DrawTextBlock(L"FeatherCast", {16, height - 28.0f, 160, height - 12.0f}, footerFormat_.Get(), D2DColor(theme_.textPrimary));
      if (overlayStatus_) {
        const D2D1_COLOR_F statusColor =
            overlayStatus_->severity == StatusSeverity::Error
                ? D2DColor(theme_.danger)
                : D2DColor(theme_.textMuted);
        DrawTextBlock(overlayStatus_->text,
                      {200, height - 28.0f, width - 16.0f,
                       height - 12.0f},
                      footerRightFormat_.Get(), statusColor);
      } else {
        DrawTextBlock(
            L"Up/Down Navigate | Enter Open | Tab Actions | Ctrl+Space Preview | Esc Close",
            {200, height - 28.0f, width - 16.0f, height - 12.0f},
            footerRightFormat_.Get(), D2DColor(theme_.textMuted));
      }
    }
  }

  static std::pair<RectF, RectF> ConfirmationButtonRects(float width, float height) {
    const float dialogWidth = std::min(460.0f, width - 40.0f);
    const float dialogLeft = (width - dialogWidth) * 0.5f;
    const float dialogTop = (height - 210.0f) * 0.5f;
    const float buttonTop = dialogTop + 144.0f;
    const float buttonWidth = (dialogWidth - 52.0f) * 0.5f;
    return {
      {dialogLeft + 20.0f, buttonTop, dialogLeft + 20.0f + buttonWidth, buttonTop + 42.0f},
      {dialogLeft + 32.0f + buttonWidth, buttonTop, dialogLeft + dialogWidth - 20.0f, buttonTop + 42.0f},
    };
  }

  void DrawConfirmationDialog() {
    if (!confirmation_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetWindowScale(hwnd_);
    const float width = static_cast<float>(rc.right - rc.left) / scale;
    const float height = static_cast<float>(rc.bottom - rc.top) / scale;
    const float dialogWidth = std::min(460.0f, width - 40.0f);
    const RectF dialog{
      (width - dialogWidth) * 0.5f,
      (height - 210.0f) * 0.5f,
      (width + dialogWidth) * 0.5f,
      (height + 210.0f) * 0.5f,
    };
    const float progress = static_cast<float>(
        std::clamp(confirmationProgress_.Value(), 0.0, 1.0));

    FillRound({0.5f, 0.5f, width - 0.5f, height - 0.5f}, theme_.overlayRadius,
              D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.58f * progress));
    D2D1_MATRIX_3X2_F dialogBase;
    activeRT_->GetTransform(&dialogBase);
    const D2D1_POINT_2F dialogCenter = D2D1::Point2F(
        (dialog.left + dialog.right) * 0.5f,
        (dialog.top + dialog.bottom) * 0.5f);
    const float dialogScale = SpatialAnimationsAllowed()
                                  ? 0.98f + 0.02f * progress
                                  : 1.0f;
    const float dialogOffset =
        SpatialAnimationsAllowed() ? 4.0f * (1.0f - progress) : 0.0f;
    activeRT_->SetTransform(
        D2D1::Matrix3x2F::Scale(dialogScale, dialogScale, dialogCenter) *
        D2D1::Matrix3x2F::Translation(0.0f, dialogOffset) *
        dialogBase);
    activeRT_->PushLayer(
        D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                              D2D1::IdentityMatrix(), progress),
        nullptr);
    FillRound(dialog, theme_.overlayRadius, D2DColor(theme_.overlayBackground));
    StrokeRound(dialog, theme_.overlayRadius, D2DColor(theme_.border));

    DrawTextBlock(confirmation_->title,
                  {dialog.left + 24.0f, dialog.top + 24.0f, dialog.right - 24.0f, dialog.top + 50.0f},
                  titleFormat_.Get(), D2DColor(theme_.textPrimary));
    DrawTextBlock(confirmation_->message,
                  {dialog.left + 24.0f, dialog.top + 62.0f, dialog.right - 24.0f, dialog.top + 92.0f},
                  bodyFormat_.Get(), D2DColor(theme_.textMuted));

    const auto [cancelRect, actionRect] = ConfirmationButtonRects(width, height);
    const COLORREF accent = ActiveAccent();
    const bool cancelHover = confirmationHover_ == 0;
    const bool actionHover = confirmationHover_ == 1;

    FillRound(cancelRect, theme_.controlRadius,
              cancelHover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.surface));
    StrokeRound(cancelRect, theme_.controlRadius,
                confirmationFocus_ == 0 ? D2DColor(accent) : D2DColor(theme_.border),
                confirmationFocus_ == 0 ? 2.0f : 1.0f);
    DrawTextBlock(L"Cancel", cancelRect, centerFormat_.Get(), D2DColor(theme_.textPrimary));

    D2D1_COLOR_F danger = D2DColor(theme_.danger);
    if (actionHover) {
      danger.r = std::min(1.0f, danger.r + 0.08f);
      danger.g = std::min(1.0f, danger.g + 0.04f);
      danger.b = std::min(1.0f, danger.b + 0.04f);
    }
    FillRound(actionRect, theme_.controlRadius, danger);
    if (confirmationFocus_ == 1) {
      StrokeRound(actionRect, theme_.controlRadius, D2D1::ColorF(1, 1, 1), 2.0f);
    }
    DrawTextBlock(confirmation_->actionLabel, actionRect, centerFormat_.Get(), D2D1::ColorF(1, 1, 1));

    if (!confirmationClosing_ && progress >= 0.999f) {
      hits_.push_back({cancelRect, HitType::ConfirmCancel});
      hits_.push_back({actionRect, HitType::ConfirmAction});
    }
    activeRT_->PopLayer();
    activeRT_->SetTransform(dialogBase);
  }

  // Keep the glass surface and search controls stationary while app rows fade
  // and rise gently into place one after another. Only rows move, so the overlay
  // itself cannot produce the doubled-panel effect of the previous reveal.
  static constexpr double kRowAnimDurationMs = 140.0;
  static constexpr double kRowAnimStaggerMs = 14.0;
  static constexpr int kRowAnimMaxStaggerRows = 5;
  static constexpr float kRowAnimSlidePx = 6.0f;

  struct RowAnim { float opacity; float dy; };

  RowAnim ComputeRowAnim(int rowIndex) const {
    if (!animating_) return {1.0f, 0.0f};
    const double delay =
        std::min(rowIndex, kRowAnimMaxStaggerRows) * kRowAnimStaggerMs;
    const double revealElapsedMs =
        overlayRevealTimeline_.ElapsedSeconds() * 1000.0;
    double t = (revealElapsedMs - delay) / kRowAnimDurationMs;
    t = std::clamp(t, 0.0, 1.0);
    const double eased = 1.0 - std::pow(1.0 - t, 3.0);
    return {
        static_cast<float>(eased),
        SpatialAnimationsAllowed()
            ? static_cast<float>((eased - 1.0) * kRowAnimSlidePx)
            : 0.0f,
    };
  }

  double AnimFinishMs() const {
    return kRowAnimMaxStaggerRows * kRowAnimStaggerMs + kRowAnimDurationMs;
  }

  void DrawSelectionPill(float width) {
    if (!ResultsActivationAllowed()) return;
    const auto targetY = SelectedRowTop();
    if (!targetY) return;
    if (!SpatialAnimationsAllowed() || visualSelectedY_ < 0.0f ||
        overlayVisualScroll_.Active() || HasElementMotion(resultRowMotion_)) {
      visualSelectedY_ = *targetY;
      animatingSelection_ = false;
    }

    const COLORREF accent = ActiveAccent();
    RectF rect{8.0f, visualSelectedY_, width - 8.0f, visualSelectedY_ + kResultRowHeight};
    const RowAnim anim = ComputeRowAnim(selected_);
    if (anim.opacity < 1.0f || anim.dy != 0.0f) {
      D2D1_MATRIX_3X2_F base;
      activeRT_->GetTransform(&base);
      activeRT_->SetTransform(
          D2D1::Matrix3x2F::Translation(0.0f, anim.dy) * base);
      activeRT_->PushLayer(
          D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                D2D1::IdentityMatrix(), anim.opacity),
          nullptr);
      FillRound(rect, theme_.rowRadius,
                Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.20f, 1.0f));
      activeRT_->PopLayer();
      activeRT_->SetTransform(base);
    } else {
      FillRound(rect, theme_.rowRadius,
                Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.20f, 1.0f));
    }
  }

  const DisplayItem* SelectedMarkdownDetailItem() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(flatItems_.size())) return nullptr;
    const auto& item = flatItems_[static_cast<size_t>(selected_)];
    if (!item.isExtension || item.extension.detailBody.empty()) return nullptr;
    return Lower(item.extension.detailType) == L"markdown" ? &item : nullptr;
  }

  float MarkdownDetailWidth(float width) const {
    if (!SelectedMarkdownDetailItem() || width < 680.0f) return 0.0f;
    return std::clamp(width * 0.38f, 240.0f, 320.0f);
  }

  static std::wstring StripInlineCodeMarkers(std::wstring text) {
    text.erase(std::remove(text.begin(), text.end(), L'`'), text.end());
    return text;
  }

  void DrawMarkdownDetail(const DisplayItem& item, RectF rect) {
    FillRound(rect, theme_.rowRadius, D2DColor(theme_.surface));
    StrokeRound(rect, theme_.rowRadius, D2DColor(theme_.border));

    const float left = rect.left + 12.0f;
    const float right = rect.right - 12.0f;
    float y = rect.top + 12.0f;
    const std::wstring title = item.extension.detailTitle.empty() ? item.Name() : item.extension.detailTitle;
    DrawTextBlock(title, {left, y, right, y + 24.0f}, rowFormat_.Get(), D2DColor(theme_.textPrimary));
    y += 30.0f;

    std::wistringstream lines(item.extension.detailBody);
    std::wstring line;
    bool codeBlock = false;
    while (std::getline(lines, line) && y < rect.bottom - 12.0f) {
      if (!line.empty() && line.back() == L'\r') line.pop_back();
      std::wstring trimmed = Trim(line);
      if (StartsWith(trimmed, L"```")) {
        codeBlock = !codeBlock;
        y += codeBlock ? 4.0f : 6.0f;
        continue;
      }
      if (trimmed.empty()) {
        y += 8.0f;
        continue;
      }

      if (codeBlock) {
        FillRound({left - 4.0f, y - 1.0f, right + 4.0f, y + 20.0f}, 4.0f, D2DColor(theme_.iconTile));
        DrawTextBlock(trimmed, {left, y + 1.0f, right, y + 19.0f}, subFormat_.Get(), D2DColor(theme_.textPrimary));
        y += 23.0f;
        continue;
      }

      IDWriteTextFormat* format = subFormat_.Get();
      D2D1_COLOR_F color = D2DColor(theme_.textMuted);
      float lineHeight = 19.0f;
      if (StartsWith(trimmed, L"#")) {
        size_t pos = 0;
        while (pos < trimmed.size() && trimmed[pos] == L'#') ++pos;
        trimmed = Trim(trimmed.substr(pos));
        format = rowFormat_.Get();
        color = D2DColor(theme_.textPrimary);
        lineHeight = 23.0f;
      } else if (StartsWith(trimmed, L"- ") || StartsWith(trimmed, L"* ")) {
        trimmed = L"- " + Trim(trimmed.substr(2));
        color = D2DColor(theme_.textPrimary);
      } else {
        trimmed = StripInlineCodeMarkers(std::move(trimmed));
      }
      DrawTextBlock(trimmed, {left, y, right, y + lineHeight}, format, color);
      y += lineHeight;
    }
  }

  void DrawResultRow(const DisplayItem& item, RectF rowRect, int rowIndex) {
    const bool selected = rowIndex == selected_;
    if (ResultsActivationAllowed()) {
      hits_.push_back({rowRect, HitType::Result, rowIndex});
    }

    if (item.isSymbol) {
      // Draw the symbol/emoji itself, large, in the left tile - the only place it
      // appears now (name and subtitle no longer repeat it). Drawing the whole
      // value string (not a single UTF-16 unit) keeps surrogate-pair emoji intact.
      const float box = 34.0f;
      const float bx = rowRect.left + 10;
      // Centre the tile within the new 50px row height.
      const float by = rowRect.top + (kResultRowHeight - box) / 2.0f;
      FillRound({bx, by, bx + box, by + box}, 8, D2DColor(theme_.iconTile));
      DrawTextBlock(item.symbol.value, {bx, by, bx + box, by + box}, emojiFormat_.Get(), D2DColor(theme_.textPrimary));
      // Unified text start at +52px (was +56 for emoji path) to eliminate jitter.
      DrawTextBlock(item.Name(), {rowRect.left + 52, rowRect.top + 8, rowRect.right - 180, rowRect.top + 28}, rowFormat_.Get(), D2DColor(theme_.textPrimary));
      DrawTextBlock(SourceLabel(item), {rowRect.left + 52, rowRect.top + 29, rowRect.right - 180, rowRect.bottom}, subFormat_.Get(), D2DColor(theme_.textMuted));
      if (selected && rowRect.right - rowRect.left > 520.0f) {
        DrawTextBlock(ActionHint(item), {rowRect.right - 330, rowRect.top + 16, rowRect.right - 10, rowRect.bottom}, footerRightFormat_.Get(), D2DColor(theme_.textMuted));
      }
      return;
    }

    const float iconX = rowRect.left + 12;
    const float iconY = rowRect.top + 11;
    auto bitmap = IconBitmap(item.IconKey());
    if (bitmap) {
      activeRT_->DrawBitmap(bitmap.Get(), D2D1::RectF(iconX, iconY, iconX + 24, iconY + 24), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      FillRound({iconX, iconY, iconX + 24, iconY + 24}, 6, D2DColor(theme_.iconTile));
      DrawResultIcon(feathercast::ui::ResolveResultIcon(item),
                     {iconX, iconY, iconX + 24, iconY + 24},
                     D2DColor(ActiveAccent()));
    }

    if (item.isCalculator || item.isConversion) {
      constexpr float kTextGap = 16.0f;
      const float contentLeft = rowRect.left + 52.0f;
      const float contentRight = rowRect.right - 10.0f;
      const float contentWidth = std::max(0.0f, contentRight - contentLeft);
      const float resultWidth =
          std::clamp(contentWidth * 0.42f, 160.0f, 300.0f);
      const float resultLeft = contentRight - resultWidth;
      const float expressionRight = std::max(contentLeft, resultLeft - kTextGap);

      // Raycast-style calculation rows keep the original expression on the
      // left and reserve a stable, right-aligned column for the answer. The
      // disjoint rectangles ensure long expressions are clipped before they
      // can overlap the result.
      DrawTextBlock(item.calculationExpression,
                    {contentLeft, rowRect.top + 8.0f, expressionRight,
                     rowRect.top + 28.0f},
                    rowFormat_.Get(), D2DColor(theme_.textPrimary));
      DrawTextBlock(item.calculationResult,
                    {resultLeft, rowRect.top + 7.0f, contentRight,
                     rowRect.top + 29.0f},
                    calculationResultFormat_.Get(),
                    D2DColor(theme_.textPrimary));
      if (selected) {
        DrawTextBlock(ActionHint(item),
                      {resultLeft, rowRect.top + 29.0f, contentRight,
                       rowRect.bottom},
                      footerRightFormat_.Get(), D2DColor(theme_.textMuted));
      }
      return;
    }

    // Unified text start at +52px (was +50 for icon path) to eliminate horizontal jitter.
    DrawTextBlock(item.Name(), {rowRect.left + 52, rowRect.top + 8, rowRect.right - 180, rowRect.top + 28}, rowFormat_.Get(), D2DColor(theme_.textPrimary));
    DrawTextBlock(SourceLabel(item), {rowRect.left + 52, rowRect.top + 29, rowRect.right - 180, rowRect.bottom}, subFormat_.Get(), D2DColor(theme_.textMuted));
    if (selected && rowRect.right - rowRect.left > 520.0f) {
      DrawTextBlock(ActionHint(item), {rowRect.right - 330, rowRect.top + 16, rowRect.right - 10, rowRect.bottom}, footerRightFormat_.Get(), D2DColor(theme_.textMuted));
    }
  }

  std::wstring SourceLabel(const DisplayItem& item) const {
    if (item.isCapability) {
      return item.capability.example.empty()
                 ? item.capability.summary
                 : item.capability.summary + L" Example: " +
                       item.capability.example;
    }
    if (item.isCalculator || item.isConversion) return item.commandDetail;
    if (item.isWebSearch) return item.commandDetail;
    if (item.isExtension) return item.commandDetail;
    if (item.isSnippet) return L"Snippet - " + item.snippet.keyword;
    if (item.isClipboard) return L"Clipboard History";
    if (item.isRunCommand) return item.commandDetail;
    if (item.isSymbol) return L"Symbol - paste";
    if (item.utility) return L"Local utility - " + item.utility->value;
    if (item.isCommand || item.isAction) return item.commandDetail;
    if (item.isWindow) return item.window.processName.empty() ? L"Open window" : L"Open window - " + item.window.processName;
    if (item.app.isGame) {
      return item.app.gameProvider.empty() ? L"Installed game"
                                           : item.app.gameProvider + L" game";
    }
    if (item.app.source == L"shortcut") return L"Desktop app";
    if (item.app.source == L"quicklink") return L"Quicklink";
    if (item.app.source == L"file") {
      if (item.app.fileContentMatch) {
        return item.app.path.empty() ? L"Content match"
                                     : L"Content match - " + item.app.path;
      }
      return item.app.path.empty() ? L"File or folder" : item.app.path;
    }
    if (item.app.source == L"system-folder") return item.app.path.empty() ? L"System folder" : item.app.path;
    if (item.app.source == L"windows-settings") return L"Windows Settings";
    if (item.app.source == L"alias") return L"System app";
    if (item.app.source == L"appx") return L"Store/System app";
    return L"Start menu app";
  }

  std::wstring ActionHint(const DisplayItem& item) const {
    if (item.isCapability) {
      switch (item.capability.action.kind) {
        case CapabilityActionKind::SeedQuery: return L"Enter Try";
        case CapabilityActionKind::OpenBrowse: return L"Enter Browse";
        case CapabilityActionKind::OpenSettings: return L"Enter Settings";
        case CapabilityActionKind::RunCommand: return L"Enter Open";
      }
    }
    if (item.isCalculator || item.isConversion) return L"Enter Copy";
    if (item.isWebSearch) return L"Enter Open";
    if (item.isExtension) return L"Enter Run";
    if (item.isSnippet || item.isClipboard) return L"Enter Paste";
    if (item.isRunCommand) return item.runCommand.kind == feathercast::run_command::Kind::OpenTarget ? L"Enter Open" : L"Enter Run";
    if (item.isSymbol) return L"Enter Paste";
    if (item.utility) return L"Enter Copy";
    if (item.isCommand) return L"Enter Run";
    if (item.isAction) return L"Enter Apply";
    if (item.isWindow) return L"Enter Switch";
    return L"Enter Open | Ctrl+Shift Admin | Ctrl+K Actions";
  }

  static constexpr float kSettTop = 72.0f;
  static constexpr float kSettSidebarWidth = 180.0f;
  static constexpr float kSettContentInset = 24.0f;
  static constexpr float kSettCategoryRow = 44.0f;
  static constexpr float kSettSection = 34.0f;
  static constexpr float kSettRow = 60.0f;
  static constexpr float kSettShortcut = 98.0f;
  static constexpr float kSettMaint = 58.0f;
  static constexpr float kSettBottom = 22.0f;
  static constexpr float kSettStatus = 52.0f;

  static constexpr std::array<SettingsCategory, 8> kSettingsCategories = {
    SettingsCategory::Shortcut,
    SettingsCategory::General,
    SettingsCategory::Results,
    SettingsCategory::Library,
    SettingsCategory::Privacy,
    SettingsCategory::Extensions,
    SettingsCategory::Appearance,
    SettingsCategory::Maintenance,
  };

  static int SettingsCategoryIndex(SettingsCategory category) {
    const auto it = std::find(kSettingsCategories.begin(), kSettingsCategories.end(), category);
    return it == kSettingsCategories.end()
               ? 0
               : static_cast<int>(std::distance(kSettingsCategories.begin(), it));
  }

  static const wchar_t* SettingsCategoryLabel(SettingsCategory category) {
    const auto* descriptor =
        feathercast::settings_catalog::FindCategory(category);
    return descriptor ? descriptor->label.data() : L"Settings";
  }

  static HitType SettingsCategoryHit(SettingsCategory category) {
    const auto* descriptor =
        feathercast::settings_catalog::FindCategory(category);
    return descriptor ? descriptor->hit : HitType::SettingsGeneralCategory;
  }

  static std::optional<SettingsCategory> SettingsCategoryForHit(HitType type) {
    const auto* descriptor =
        feathercast::settings_catalog::FindCategory(type);
    return descriptor ? std::optional{descriptor->category} : std::nullopt;
  }

  float SettingsStatusOffset() const {
    return settingsStatus_ ? kSettStatus : 0.0f;
  }

  void SetSettingsStatus(StatusSeverity severity, std::wstring text) {
    settingsStatus_ = StatusMessage{severity, std::move(text)};
    if (settingsHwnd_) {
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, settingsHwnd_, OBJID_CLIENT,
                     CHILDID_SELF);
    }
  }

  void SetOverlayStatus(StatusSeverity severity, std::wstring text) {
    overlayStatus_ = StatusMessage{severity, std::move(text)};
    if (hwnd_) {
      InvalidateRect(hwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd_, OBJID_CLIENT,
                     CHILDID_SELF);
    }
  }

  bool SettingsControlEnabled(HitType type) const {
    const auto* descriptor = feathercast::settings_catalog::Find(type);
    if (!descriptor) return true;
    return feathercast::settings_catalog::Enabled(
        *descriptor, SettingsCatalogContext());
  }

  feathercast::settings_catalog::CatalogContext SettingsCatalogContext() const {
    feathercast::settings_catalog::CatalogContext context;
    context.hasPendingShortcut = !pendingShortcut_.empty();
    context.hasExistingShortcut =
        !settings_.shortcut.empty() && settings_.shortcut != L"none";
    context.clipboardEnabled = settings_.clipboardHistoryEnabled;
    context.fileIndexEnabled = settings_.fileIndexEnabled;
    context.storageIdle = !pendingStorageOperation_.has_value();
    context.extensionsIdle = !extensionReloadPending_;
    context.customAccent = !settings_.syncAccentColor;
    return context;
  }

  int SettingsPageContentHeight() const {
    float h = kSettTop + SettingsStatusOffset();
    switch (settingsCategory_) {
      case SettingsCategory::Shortcut:
        h += kSettSection + kSettShortcut;
        break;
      case SettingsCategory::General:
      case SettingsCategory::Results:
        h += kSettSection + 4 * kSettRow;
        break;
      case SettingsCategory::Library:
        h += kSettSection + 2 * kSettRow;
        break;
      case SettingsCategory::Privacy:
        h += kSettSection + 6 * kSettRow + 3 * kSettMaint;
        break;
      case SettingsCategory::Extensions:
        h += kSettSection + kSettRow +
             static_cast<float>(extensions_.Health().size()) * kSettRow +
             kSettMaint;
        break;
      case SettingsCategory::Appearance:
        h += kSettSection + kSettRow + (settings_.syncAccentColor ? 0.0f : 48.0f);
        break;
      case SettingsCategory::Maintenance:
        h += kSettSection + kSettMaint;
        break;
    }
    h += kSettBottom;
    return static_cast<int>(h);
  }

  bool SettHover(HitType type) const { return settingsHover_ == static_cast<int>(type); }
  bool SettFocused(HitType type) const {
    const auto order = SettingsFocusOrder();
    return settingsFocusIndex_ >= 0 && settingsFocusIndex_ < static_cast<int>(order.size()) &&
           order[static_cast<size_t>(settingsFocusIndex_)] == type;
  }

  std::vector<HitType> SettingsFocusOrder() const {
    std::vector<HitType> order = {
      HitType::SettingsShortcutCategory,
      HitType::SettingsGeneralCategory,
      HitType::SettingsResultsCategory,
      HitType::SettingsLibraryCategory,
      HitType::SettingsPrivacyCategory,
      HitType::SettingsExtensionsCategory,
      HitType::SettingsAppearanceCategory,
      HitType::SettingsMaintenanceCategory,
    };
    auto controls = feathercast::settings_catalog::FocusOrder(
        settingsCategory_, SettingsCatalogContext());
    order.insert(order.end(), controls.begin(), controls.end());
    order.push_back(HitType::CloseSettings);
    return order;
  }

  D2D1_COLOR_F SettWhite() const { return D2DColor(theme_.textPrimary); }
  D2D1_COLOR_F SettGray() const { return D2DColor(theme_.textMuted); }

  float DrawSettingsSection(float y, const std::wstring& title, float left, float right) {
    DrawTextBlock(title, {left, y + 12, right, y + 30}, sectionFormat_.Get(),
                  D2DColor(theme_.sectionText));
    auto border = Brush(D2DColor(theme_.divider));
    activeRT_->DrawLine(D2D1::Point2F(left, y + kSettSection - 2),
                        D2D1::Point2F(right, y + kSettSection - 2),
                        border.Get(), 1);
    return y + kSettSection;
  }

  void DrawSettingsStatus(float left, float right) {
    if (!settingsStatus_) return;
    const RectF banner{left, kSettTop, right, kSettTop + 40.0f};
    D2D1_COLOR_F border = D2DColor(ActiveAccent());
    switch (settingsStatus_->severity) {
      case StatusSeverity::Error:
        border = D2DColor(theme_.danger);
        break;
      case StatusSeverity::Success:
        border = D2D1::ColorF(0.30f, 0.78f, 0.48f, 1.0f);
        break;
      case StatusSeverity::Progress:
      case StatusSeverity::Info:
        break;
    }
    D2D1_COLOR_F fill = D2DColor(theme_.surface);
    fill.a = 0.92f;
    FillRound(banner, theme_.controlRadius, fill);
    StrokeRound(banner, theme_.controlRadius, border,
                settingsStatus_->severity == StatusSeverity::Error ? 2.0f
                                                                  : 1.0f);
    FillRound({banner.left + 10.0f, banner.top + 10.0f, banner.left + 14.0f,
               banner.bottom - 10.0f},
              2.0f, border);
    DrawTextBlock(settingsStatus_->text,
                  {banner.left + 24.0f, banner.top + 9.0f,
                   banner.right - 12.0f, banner.bottom - 7.0f},
                  bodyFormat_.Get(), SettWhite());
  }

  void DrawSettingRowLabel(float y, float rowH, const std::wstring& label,
                           const std::wstring& desc, float left, float labelRight,
                           bool enabled = true) {
    DrawTextBlock(label, {left, y + rowH / 2 - 20, labelRight, y + rowH / 2},
                  labelFormat_.Get(),
                  enabled ? SettWhite() : D2DColor(theme_.textDim));
    DrawTextBlock(desc, {left, y + rowH / 2, labelRight, y + rowH / 2 + 20},
                  bodyFormat_.Get(),
                  enabled ? SettGray() : D2DColor(theme_.textDim));
  }

  void DrawCatalogSettingRowLabel(
      float y, float rowH, HitType type, float left, float labelRight,
      bool enabled = true, const std::wstring& descriptionOverride = L"") {
    const auto* descriptor = feathercast::settings_catalog::Find(type);
    if (!descriptor) return;
    const std::wstring description =
        descriptionOverride.empty()
            ? std::wstring(descriptor->description)
            : descriptionOverride;
    DrawSettingRowLabel(y, rowH, std::wstring(descriptor->label), description,
                        left, labelRight, enabled);
  }

  void DrawSwitch(float y, float rowH, bool on, HitType type, float left,
                  float right, bool enabled = true) {
    const COLORREF accent = ActiveAccent();
    const float trackLeft = right - 46.0f;
    const float h = 26.0f;
    const float top = y + (rowH - h) / 2.0f;
    RectF track{trackLeft, top, right, top + h};
    const bool hover = enabled && SettHover(type);
    auto [animation, inserted] = switchAnimations_.try_emplace(type);
    if (inserted) {
      animation->second.Snap(on ? 1.0 : 0.0);
    } else if (std::abs(animation->second.Target() - (on ? 1.0 : 0.0)) >
               0.001) {
      animation->second.Retarget(on ? 1.0 : 0.0, 0.110,
                                 ControlAnimationsAllowed());
      if (animation->second.Active()) RequestAnimationFrame();
    }
    const float progress = static_cast<float>(
        std::clamp(animation->second.Value(), 0.0, 1.0));
    const D2D1_COLOR_F offColor =
        hover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.surface);
    const D2D1_COLOR_F onColor = D2DColor(accent);
    D2D1_COLOR_F trackColor = D2D1::ColorF(
        offColor.r + (onColor.r - offColor.r) * progress,
        offColor.g + (onColor.g - offColor.g) * progress,
        offColor.b + (onColor.b - offColor.b) * progress,
        offColor.a + (onColor.a - offColor.a) * progress);
    if (!enabled) trackColor.a *= 0.45f;
    FillRound(track, 13, trackColor);
    if (SettFocused(type)) StrokeRound(track, 13, D2DColor(accent), 2.0f);
    const float knobX = track.left + 4.0f +
                        (track.right - 22.0f - (track.left + 4.0f)) *
                            progress;
    auto knobColor = D2D1::ColorF(1, 1, 1);
    if (!enabled) knobColor.a = 0.55f;
    FillRound({knobX, top + 4, knobX + 18, top + 22}, 9, knobColor);
    hits_.push_back({{left - 8.0f, y, right, y + rowH}, type});
  }

  static RectF AnimationSliderHitRect(float y, float rowH, float right) {
    return {right - 232.0f, y + 4.0f, right, y + rowH - 4.0f};
  }

  static RectF AnimationSliderTrackRect(const RectF& hit) {
    return {hit.left + 16.0f, hit.top + 18.0f, hit.right - 16.0f,
            hit.top + 22.0f};
  }

  void DrawAnimationSlider(float y, float rowH, float right) {
    const RectF hit = AnimationSliderHitRect(y, rowH, right);
    const RectF track = AnimationSliderTrackRect(hit);
    const COLORREF accent = ActiveAccent();
    const int selected = static_cast<int>(settings_.animationLevel);
    const bool hover = SettHover(HitType::AnimationLevel);

    if (hover) {
      FillRound(hit, theme_.controlRadius, D2DColor(theme_.surfaceHover));
    }
    if (SettFocused(HitType::AnimationLevel)) {
      StrokeRound(hit, theme_.controlRadius, D2DColor(accent), 2.0f);
    }

    FillRound(track, 2.0f, D2DColor(theme_.surface));
    const float selectedX =
        track.left + (track.right - track.left) * selected / 2.0f;
    if (selected > 0) {
      FillRound({track.left, track.top, selectedX, track.bottom}, 2.0f,
                D2DColor(accent));
    }

    constexpr std::array levels{
        AnimationLevel::Off,
        AnimationLevel::Reduced,
        AnimationLevel::Full,
    };
    const float segmentWidth = (hit.right - hit.left) / 3.0f;
    for (int index = 0; index < static_cast<int>(levels.size()); ++index) {
      const float stopX =
          track.left + (track.right - track.left) * index / 2.0f;
      const bool active = index == selected;
      FillRound({stopX - (active ? 8.0f : 4.0f),
                 (track.top + track.bottom) * 0.5f - (active ? 8.0f : 4.0f),
                 stopX + (active ? 8.0f : 4.0f),
                 (track.top + track.bottom) * 0.5f + (active ? 8.0f : 4.0f)},
                active ? 8.0f : 4.0f,
                active ? D2DColor(accent) : D2DColor(theme_.textDim));
      DrawTextBlock(
          std::wstring(feathercast::settings::AnimationLevelLabel(levels[index])),
          {hit.left + segmentWidth * index, hit.top + 31.0f,
           hit.left + segmentWidth * (index + 1), hit.bottom},
          centerFormat_.Get(), active ? D2DColor(accent) : SettGray());
    }
    hits_.push_back({hit, HitType::AnimationLevel});
  }

  void DrawStepper(float y, float rowH, const std::wstring& value,
                   HitType down, HitType up, float right, bool enabled = true) {
    const float cy = y + rowH / 2.0f;
    RectF plus{right - 36, cy - 17, right, cy + 17};
    RectF minus{plus.left - 8 - 36, cy - 17, plus.left - 8, cy + 17};
    DrawSettingsButton(minus, L"-", down, enabled);
    DrawSettingsButton(plus, L"+", up, enabled);
    DrawTextBlock(value, {minus.left - 96, cy - 11, minus.left - 12, cy + 11},
                  footerRightFormat_.Get(),
                  enabled ? SettWhite() : D2DColor(theme_.textDim));
  }

  void DrawSettingsSidebar(float height) {
    const COLORREF accent = ActiveAccent();
    for (size_t i = 0; i < kSettingsCategories.size(); ++i) {
      const SettingsCategory category = kSettingsCategories[i];
      const HitType type = SettingsCategoryHit(category);
      const float top = kSettTop + static_cast<float>(i) * kSettCategoryRow;
      const RectF rect{12.0f, top, kSettSidebarWidth - 12.0f, top + 38.0f};
      const bool selected = category == settingsCategory_;
      const bool hover = SettHover(type);
      if (selected) {
        const float selectedTop =
            static_cast<float>(settingsCategoryTop_.Value());
        const RectF animatedRect{rect.left, selectedTop, rect.right,
                                 selectedTop + 38.0f};
        FillRound(animatedRect, theme_.controlRadius,
                  Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.22f));
      } else if (hover) {
        FillRound(rect, theme_.controlRadius, D2DColor(theme_.surfaceHover));
      }
      if (SettFocused(type)) {
        StrokeRound(rect, theme_.controlRadius, D2DColor(accent), 2.0f);
      }
      DrawTextBlock(SettingsCategoryLabel(category),
                    {24.0f, top + 9.0f, rect.right - 12.0f, top + 31.0f},
                    bodyFormat_.Get(), selected ? SettWhite() : SettGray());
      hits_.push_back({rect, type});
    }

    if (height > kSettTop + kSettingsCategories.size() * kSettCategoryRow + 12.0f) {
      auto divider = Brush(D2DColor(theme_.divider));
      activeRT_->DrawLine(
          D2D1::Point2F(kSettSidebarWidth, 68.0f),
          D2D1::Point2F(kSettSidebarWidth, height - 12.0f),
          divider.Get(), 1.0f);
    }
  }

  void DrawSettings() {
    RECT rc{};
    GetClientRect(settingsHwnd_, &rc);
    const float scale = GetWindowScale(settingsHwnd_);
    const float width = static_cast<float>(rc.right - rc.left) / scale;
    const float height = static_cast<float>(rc.bottom - rc.top) / scale;
    const COLORREF accent = ActiveAccent();

    DrawObsidianBackground(width, height, theme_.settingsRadius, ObsidianBackgroundKind::Settings);

    // Title bar (draggable region).
    DrawTextBlock(L"Settings", {24, 16, width - 60, 46}, titleFormat_.Get(), SettWhite());
    DrawTextBlock(L"Drag this bar to move", {24, 38, width - 60, 54}, bodyFormat_.Get(), D2DColor(theme_.textDim));
    RectF closeBtn{width - 46, 12, width - 14, 44};
    const bool closeHover = SettHover(HitType::CloseSettings);
    FillRound(closeBtn, theme_.controlRadius, closeHover ? D2DColor(theme_.danger) : D2DColor(theme_.divider));
    {
      auto xBrush = Brush(closeHover ? D2D1::ColorF(1, 1, 1) : SettGray());
      const float cx = (closeBtn.left + closeBtn.right) / 2.0f;
      const float cyc = (closeBtn.top + closeBtn.bottom) / 2.0f;
      activeRT_->DrawLine(D2D1::Point2F(cx - 6, cyc - 6), D2D1::Point2F(cx + 6, cyc + 6), xBrush.Get(), 2.0f);
      activeRT_->DrawLine(D2D1::Point2F(cx - 6, cyc + 6), D2D1::Point2F(cx + 6, cyc - 6), xBrush.Get(), 2.0f);
    }
    hits_.push_back({closeBtn, HitType::CloseSettings});

    DrawSettingsSidebar(height);

    // Keep the scroll offset within range (e.g. after a DPI/height change) and
    // clip the active page so it never paints over the title bar or sidebar.
    const float maxScroll =
        std::max(0.0f, static_cast<float>(SettingsPageContentHeight()) - height);
    feathercast::ui::SettingsController::SetScroll(
        settingsState_, settingsScroll_, maxScroll);
    if (std::abs(settingsVisualScroll_.Target() - settingsScroll_) > 0.001) {
      RetargetSettingsScroll();
    }
    activeRT_->PushAxisAlignedClip(
        D2D1::RectF(kSettSidebarWidth + 1.0f, 57.0f, width, height),
        D2D1_ANTIALIAS_MODE_ALIASED);
    const float pageProgress = static_cast<float>(
        std::clamp(settingsPageProgress_.Value(), 0.0, 1.0));
    D2D1_MATRIX_3X2_F pageBase;
    activeRT_->GetTransform(&pageBase);
    activeRT_->SetTransform(
        D2D1::Matrix3x2F::Translation(0.0f,
                                     SpatialAnimationsAllowed()
                                         ? (1.0f - pageProgress) * 4.0f
                                         : 0.0f) *
        pageBase);
    const bool pageLayer = pageProgress < 0.999f;
    if (pageLayer) {
      activeRT_->PushLayer(
          D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                D2D1::IdentityMatrix(), pageProgress),
          nullptr);
    }

    const float contentLeft = kSettSidebarWidth + kSettContentInset;
    const float contentRight = width - kSettContentInset;
    const float contentWidth = contentRight - contentLeft;
    DrawSettingsStatus(contentLeft, contentRight);
    float y = kSettTop + SettingsStatusOffset() -
              static_cast<float>(settingsVisualScroll_.Value());

    switch (settingsCategory_) {
      case SettingsCategory::Shortcut: {
        y = DrawSettingsSection(y, L"GLOBAL SHORTCUT", contentLeft, contentRight);
        const std::wstring current =
            settings_.shortcut.empty() || settings_.shortcut == L"none"
                ? L"None"
                : settings_.shortcut;
        DrawTextBlock(L"Open FeatherCast",
                      {contentLeft, y + 12, contentRight, y + 32},
                      labelFormat_.Get(), SettWhite());
        DrawTextBlock(L"Current: " + current + L". At least one modifier required.",
                      {contentLeft, y + 33, contentRight, y + 51},
                      bodyFormat_.Get(), SettGray());
        const float btnTop = y + 56;
        RectF record{contentLeft, btnTop, contentRight - 106, btnTop + 38};
        const bool recHover = SettHover(HitType::RecordShortcut);
        FillRound(
            record, theme_.controlRadius,
            recording_
                ? Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.28f)
                : (recHover ? D2DColor(theme_.surfaceHover)
                            : D2DColor(theme_.surface)));
        StrokeRound(record, theme_.controlRadius,
                    recording_ ? D2DColor(accent) : D2DColor(theme_.border));
        if (SettFocused(HitType::RecordShortcut)) {
          StrokeRound(record, theme_.controlRadius, D2DColor(accent), 2.0f);
        }
        hits_.push_back({record, HitType::RecordShortcut});
        const std::wstring recordText =
            recording_ ? L"Press a key..."
                       : (!pendingShortcut_.empty() ? pendingShortcut_
                                                    : L"Record new shortcut");
        DrawTextBlock(recordText,
                      {record.left + 12, record.top + 10, record.right - 12,
                       record.bottom},
                      bodyFormat_.Get(),
                      recording_ ? D2DColor(accent) : SettWhite());
        if (!pendingShortcut_.empty()) {
          RectF save{contentRight - 94, btnTop, contentRight, btnTop + 38};
          FillRound(save, theme_.controlRadius, D2DColor(accent));
          if (SettFocused(HitType::SaveShortcut)) {
            StrokeRound(save, theme_.controlRadius, D2D1::ColorF(1, 1, 1), 2.0f);
          }
          hits_.push_back({save, HitType::SaveShortcut});
          DrawTextBlock(L"Save", save, centerFormat_.Get(), D2D1::ColorF(1, 1, 1));
        } else if (!settings_.shortcut.empty() && settings_.shortcut != L"none") {
          RectF clearBtn{contentRight - 94, btnTop, contentRight, btnTop + 38};
          StrokeRound(clearBtn, theme_.controlRadius,
                      SettFocused(HitType::ClearShortcut) ? D2DColor(accent)
                                                          : D2DColor(theme_.danger),
                      SettFocused(HitType::ClearShortcut) ? 2.0f : 1.0f);
          hits_.push_back({clearBtn, HitType::ClearShortcut});
          DrawTextBlock(L"Clear", clearBtn, centerFormat_.Get(),
                        D2DColor(theme_.danger));
        }
        break;
      }
      case SettingsCategory::General:
        y = DrawSettingsSection(y, L"GENERAL", contentLeft, contentRight);
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::StartupToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.startOnStartup,
                   HitType::StartupToggle, contentLeft, contentRight);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::UpdateChecksToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.updateChecksEnabled,
                   HitType::UpdateChecksToggle, contentLeft, contentRight);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::CompactToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.compactMode,
                   HitType::CompactToggle, contentLeft, contentRight);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::AnimationLevel,
                                   contentLeft, contentRight - 252);
        DrawAnimationSlider(y, kSettRow, contentRight);
        break;

      case SettingsCategory::Results:
        y = DrawSettingsSection(y, L"RESULTS", contentLeft, contentRight);
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::ShowWindowsToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.showOpenWindows,
                   HitType::ShowWindowsToggle, contentLeft, contentRight);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::ShowStoreAppsToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.showStoreApps,
                   HitType::ShowStoreAppsToggle, contentLeft, contentRight);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::OverlayWidthDown,
                                   contentLeft, contentRight - 176);
        DrawStepper(y, kSettRow, std::to_wstring(OverlayWidth()) + L" px",
                    HitType::OverlayWidthDown, HitType::OverlayWidthUp,
                    contentRight);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::MaxResultsDown,
                                   contentLeft, contentRight - 176);
        DrawStepper(
            y, kSettRow,
            std::to_wstring(std::clamp(settings_.maxResults, MIN_RESULTS,
                                       MAX_RESULT_SETTING)),
            HitType::MaxResultsDown, HitType::MaxResultsUp, contentRight);
        break;

      case SettingsCategory::Library: {
        y = DrawSettingsSection(y, L"LIBRARY", contentLeft, contentRight);
        std::size_t snippetCount = 0;
        {
          std::lock_guard lock(dataMutex_);
          snippetCount = snippets_.size();
        }
        DrawCatalogSettingRowLabel(
            y, kSettRow, HitType::ManageSnippets, contentLeft,
            contentRight - 150.0f, true,
            std::to_wstring(snippetCount) +
                (snippetCount == 1 ? L" reusable text item." :
                                     L" reusable text items."));
        DrawSettingsButton(
            {contentRight - 128.0f, y + 12.0f, contentRight, y + 48.0f},
            L"Manage", HitType::ManageSnippets);
        y += kSettRow;
        const std::size_t quicklinkCount = settings_.quicklinks.size();
        DrawCatalogSettingRowLabel(
            y, kSettRow, HitType::ManageQuicklinks, contentLeft,
            contentRight - 150.0f, true,
            std::to_wstring(quicklinkCount) +
                (quicklinkCount == 1 ? L" URL, file, or folder shortcut." :
                                       L" URL, file, or folder shortcuts."));
        DrawSettingsButton(
            {contentRight - 128.0f, y + 12.0f, contentRight, y + 48.0f},
            L"Manage", HitType::ManageQuicklinks);
        break;
      }

      case SettingsCategory::Privacy: {
        y = DrawSettingsSection(y, L"PRIVACY", contentLeft, contentRight);
        DrawCatalogSettingRowLabel(y, kSettRow,
                                   HitType::ClipboardHistoryToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.clipboardHistoryEnabled,
                   HitType::ClipboardHistoryToggle, contentLeft, contentRight);
        y += kSettRow;
        const bool clipboardControls =
            SettingsControlEnabled(HitType::ClipboardLimitDown);
        DrawCatalogSettingRowLabel(y, kSettRow,
                                   HitType::ClipboardLimitDown, contentLeft,
                                   contentRight - 176, clipboardControls);
        DrawStepper(y, kSettRow, std::to_wstring(ClipboardHistoryLimit()),
                    HitType::ClipboardLimitDown, HitType::ClipboardLimitUp,
                    contentRight, clipboardControls);
        y += kSettRow;
        DrawCatalogSettingRowLabel(
            y, kSettRow, HitType::FileIndexToggle, contentLeft,
            contentRight - 66, true,
            settings_.fileIndexRoots.empty()
                ? L"Using Desktop, Documents, and Downloads."
                : std::to_wstring(settings_.fileIndexRoots.size()) +
                      L" custom folder(s).");
        DrawSwitch(y, kSettRow, settings_.fileIndexEnabled,
                   HitType::FileIndexToggle, contentLeft, contentRight);
        y += kSettRow;
        const bool fileIndexControls =
            SettingsControlEnabled(HitType::FileIndexLimitDown);
        DrawCatalogSettingRowLabel(
            y, kSettRow, HitType::FileContentIndexToggle, contentLeft,
            contentRight - 66, fileIndexControls,
            fileIndexStatus_
                ? std::to_wstring(fileIndexStatus_->indexedContentFiles) +
                      L" text files indexed locally."
                : L"Disabled by default; supported text files only.");
        DrawSwitch(y, kSettRow, settings_.fileContentIndexEnabled,
                   HitType::FileContentIndexToggle, contentLeft, contentRight,
                   fileIndexControls);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow,
                                   HitType::FileIndexLimitDown, contentLeft,
                                   contentRight - 176, fileIndexControls);
        DrawStepper(y, kSettRow, std::to_wstring(FileIndexLimit()),
                    HitType::FileIndexLimitDown, HitType::FileIndexLimitUp,
                    contentRight, fileIndexControls);
        y += kSettRow;
        DrawCatalogSettingRowLabel(y, kSettRow, HitType::DiagnosticsToggle,
                                   contentLeft, contentRight - 66);
        DrawSwitch(y, kSettRow, settings_.diagnosticsEnabled,
                   HitType::DiagnosticsToggle, contentLeft, contentRight);
        y += kSettRow;

        const float threeRootButtonWidth = (contentWidth - 24.0f) / 3.0f;
        float btnTop = y + (kSettMaint - 38) / 2.0f;
        DrawSettingsButton(
            {contentLeft, btnTop, contentLeft + threeRootButtonWidth,
             btnTop + 38},
            L"Add Indexed Folder", HitType::AddFileRoot, fileIndexControls);
        DrawSettingsButton(
            {contentLeft + threeRootButtonWidth + 12, btnTop,
             contentLeft + 2 * threeRootButtonWidth + 12, btnTop + 38},
            L"Remove Folder...", HitType::RemoveFileRoot,
            fileIndexControls);
        DrawSettingsButton(
            {contentRight - threeRootButtonWidth, btnTop, contentRight,
             btnTop + 38},
            L"Use Default Folders", HitType::ClearFileRoots,
            fileIndexControls);
        y += kSettMaint;

        btnTop = y + (kSettMaint - 38) / 2.0f;
        DrawSettingsButton(
            {contentLeft, btnTop, contentRight, btnTop + 38},
            L"Rebuild File Index", HitType::RebuildFileIndex,
            fileIndexControls);
        y += kSettMaint;

        const float threeButtonWidth = (contentWidth - 24.0f) / 3.0f;
        btnTop = y + (kSettMaint - 38) / 2.0f;
        DrawSettingsButton(
            {contentLeft, btnTop, contentLeft + threeButtonWidth, btnTop + 38},
            pendingStorageOperation_ == StorageOperationKind::ClearClipboard
                ? L"Deleting..."
                : L"Delete Clipboard Data",
            HitType::ClearClipboardData,
            SettingsControlEnabled(HitType::ClearClipboardData));
        DrawSettingsButton(
            {contentLeft + threeButtonWidth + 12, btnTop,
             contentLeft + 2 * threeButtonWidth + 12, btnTop + 38},
            pendingStorageOperation_ == StorageOperationKind::ClearFileIndex
                ? L"Deleting..."
                : L"Delete File Index",
            HitType::ClearFileIndexData,
            SettingsControlEnabled(HitType::ClearFileIndexData));
        DrawSettingsButton(
            {contentRight - threeButtonWidth, btnTop, contentRight, btnTop + 38},
            L"Open Local Data", HitType::OpenLocalDataFolder);
        break;
      }

      case SettingsCategory::Extensions: {
        y = DrawSettingsSection(y, L"EXTENSIONS", contentLeft, contentRight);
        const auto health = extensions_.Health();
        const size_t available = static_cast<size_t>(std::count_if(
            health.begin(), health.end(),
            [](const feathercast::extensions::PluginHealth& plugin) {
              return plugin.available;
            }));
        DrawSettingRowLabel(
            y, kSettRow, L"Installed Native Extensions",
            std::to_wstring(available) + L" of " +
                std::to_wstring(health.size()) +
                L" available. Only install extensions you trust.",
            contentLeft, contentRight);
        y += kSettRow;
        for (const auto& plugin : health) {
          const std::wstring state =
              !plugin.available
                  ? L"Unavailable"
                  : (plugin.failureStrikes > 0 ? L"Degraded" : L"Available");
          std::wstring detail = L"Version " + plugin.version + L" - " + state;
          if (!plugin.lastError.empty()) detail += L" - " + plugin.lastError;
          DrawSettingRowLabel(y, kSettRow, plugin.name, detail, contentLeft,
                              contentRight);
          y += kSettRow;
        }
        const float btnW = (contentWidth - 12.0f) / 2.0f;
        const float btnTop = y + (kSettMaint - 38) / 2.0f;
        DrawSettingsButton(
            {contentLeft, btnTop, contentLeft + btnW, btnTop + 38},
            extensionReloadPending_ ? L"Reloading..." : L"Reload Extensions",
            HitType::ReloadExtensions,
            SettingsControlEnabled(HitType::ReloadExtensions));
        DrawSettingsButton(
            {contentLeft + btnW + 12, btnTop, contentRight, btnTop + 38},
            L"Open Plugins Folder", HitType::OpenPluginsFolder);
        break;
      }

      case SettingsCategory::Appearance:
        y = DrawSettingsSection(y, L"APPEARANCE", contentLeft, contentRight);
        DrawCatalogSettingRowLabel(
            y, kSettRow, HitType::AccentToggle, contentLeft,
            contentRight - 66, true,
            settings_.syncAccentColor
                ? L"Match the Windows accent color automatically."
                : L"Using a custom accent color.");
        DrawSwitch(y, kSettRow, settings_.syncAccentColor,
                   HitType::AccentToggle, contentLeft, contentRight);
        y += kSettRow;
        if (!settings_.syncAccentColor) {
          RectF colorBox{contentLeft, y, contentLeft + 196, y + 36};
          const bool colorHover = SettHover(HitType::AccentColor);
          FillRound(colorBox, theme_.controlRadius, D2DColor(theme_.surface));
          StrokeRound(
              colorBox, theme_.controlRadius,
              SettFocused(HitType::AccentColor)
                  ? D2DColor(accent)
                  : (colorHover ? D2DColor(theme_.surfaceHover)
                                : D2DColor(theme_.border)),
              SettFocused(HitType::AccentColor) ? 2.0f : 1.0f);
          FillRound({contentLeft + 12, y + 9, contentLeft + 36, y + 27}, 5,
                    D2DColor(ColorRefFromHex(settings_.customAccentColor)));
          DrawTextBlock(L"Pick color  " + settings_.customAccentColor,
                        {contentLeft + 48, y + 9, contentLeft + 188, y + 29},
                        bodyFormat_.Get(), SettWhite());
          hits_.push_back({colorBox, HitType::AccentColor});
        }
        break;

      case SettingsCategory::Maintenance: {
        y = DrawSettingsSection(y, L"MAINTENANCE", contentLeft, contentRight);
        const float btnW = (contentWidth - 24.0f) / 3.0f;
        const float btnTop = y + (kSettMaint - 38) / 2.0f;
        DrawSettingsButton(
            {contentLeft, btnTop, contentLeft + btnW, btnTop + 38},
            L"Clear Recents", HitType::ClearRecents);
        DrawSettingsButton(
            {contentLeft + btnW + 12, btnTop,
             contentLeft + 2 * btnW + 12, btnTop + 38},
            L"Clear Icon Cache", HitType::ClearIconCache);
        DrawSettingsButton(
            {contentRight - btnW, btnTop, contentRight, btnTop + 38},
            L"Check Updates", HitType::CheckUpdates);
        break;
      }
    }

    if (pageLayer) activeRT_->PopLayer();
    activeRT_->SetTransform(pageBase);
    activeRT_->PopAxisAlignedClip();

    // Scrollbar thumb, shown only when the content overflows the window.
    if (maxScroll > 0.0f) {
      const float trackTop = 60.0f;
      const float trackBottom = height - 8.0f;
      const float trackH = trackBottom - trackTop;
      const float contentH = static_cast<float>(SettingsPageContentHeight());
      const float thumbH = std::max(36.0f, trackH * (height / contentH));
      const float thumbY =
          trackTop + (trackH - thumbH) *
                         (static_cast<float>(settingsVisualScroll_.Value()) /
                          maxScroll);
      FillRound({width - 7, thumbY, width - 4, thumbY + thumbH}, 1.5f, D2DColor(theme_.textDim));
    }
  }

  void DrawSettingsButton(RectF rect, const std::wstring& text, HitType type,
                          bool enabled = true) {
    const bool hover = enabled && SettHover(type);
    D2D1_COLOR_F fill =
        hover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.surface);
    if (!enabled) fill.a *= 0.45f;
    FillRound(rect, theme_.controlRadius, fill);
    StrokeRound(rect, theme_.controlRadius,
                enabled && SettFocused(type) ? D2DColor(ActiveAccent()) :
                (hover ? D2DColor(theme_.textMuted) : D2DColor(theme_.border)),
                enabled && SettFocused(type) ? 2.0f : 1.0f);
    DrawTextBlock(text, rect, centerFormat_.Get(),
                  enabled ? SettWhite() : D2DColor(theme_.textDim));
    hits_.push_back({rect, type});
  }

  // Look up the cached bitmap, promoting it to most-recently-used. The (UI-thread-only)
  // iconBitmaps_/iconLru_ pair is not guarded by iconQueueMutex_ on purpose.
  ComPtr<ID2D1Bitmap> CachedIconBitmap(const std::wstring& key) {
    auto it = iconBitmaps_.find(key);
    if (it == iconBitmaps_.end()) return nullptr;
    iconLru_.splice(iconLru_.begin(), iconLru_, it->second.lruIt);
    return it->second.bitmap;
  }

  void StoreIconBitmap(const std::wstring& key, ComPtr<ID2D1Bitmap> bitmap) {
    if (auto existing = iconBitmaps_.find(key); existing != iconBitmaps_.end()) {
      iconLru_.erase(existing->second.lruIt);
    }
    iconLru_.push_front(key);
    iconBitmaps_[key] = IconCacheEntry{std::move(bitmap), iconLru_.begin()};
    while (iconBitmaps_.size() > kIconCacheCap && !iconLru_.empty()) {
      iconBitmaps_.erase(iconLru_.back());
      iconLru_.pop_back();
    }
  }

  void ClearIconBitmaps() {
    iconBitmaps_.clear();
    iconLru_.clear();
    pendingDecodedIcons_.clear();
    pendingDecodedIconOrder_.clear();
    pendingDecodedIconBytes_ = 0;
  }

  void StorePendingDecodedIcon(
      feathercast::runtime::DecodedIcon icon) {
    if (icon.key.empty()) return;
    const auto bytes = icon.pixels.size();
    if (auto existing = pendingDecodedIcons_.find(icon.key);
        existing != pendingDecodedIcons_.end()) {
      pendingDecodedIconBytes_ -=
          std::min(pendingDecodedIconBytes_, existing->second.pixels.size());
      existing->second = std::move(icon);
    } else {
      pendingDecodedIconOrder_.push_back(icon.key);
      pendingDecodedIcons_.emplace(icon.key, std::move(icon));
    }
    pendingDecodedIconBytes_ += bytes;

    while ((pendingDecodedIcons_.size() > kPendingDecodedIconCap ||
            pendingDecodedIconBytes_ > kPendingDecodedIconBudget) &&
           !pendingDecodedIconOrder_.empty()) {
      const auto key = std::move(pendingDecodedIconOrder_.front());
      pendingDecodedIconOrder_.pop_front();
      const auto found = pendingDecodedIcons_.find(key);
      if (found == pendingDecodedIcons_.end()) continue;
      pendingDecodedIconBytes_ -=
          std::min(pendingDecodedIconBytes_, found->second.pixels.size());
      pendingDecodedIcons_.erase(found);
    }
  }

  ComPtr<ID2D1Bitmap> IconBitmap(const std::wstring& key) {
    if (key.empty() || !overlaySurface_.dc) return nullptr;
    if (auto cached = CachedIconBitmap(key)) return cached;

    if (auto decoded = pendingDecodedIcons_.find(key);
        decoded != pendingDecodedIcons_.end()) {
      const auto& icon = decoded->second;
      ComPtr<ID2D1Bitmap> bitmap;
      if (icon.width != 0 && icon.height != 0 && icon.stride != 0 &&
          !icon.pixels.empty() &&
          SUCCEEDED(overlaySurface_.dc->CreateBitmap(
              D2D1::SizeU(icon.width, icon.height), icon.pixels.data(),
              icon.stride,
              D2D1::BitmapProperties(D2D1::PixelFormat(
                  DXGI_FORMAT_B8G8R8A8_UNORM,
                  D2D1_ALPHA_MODE_PREMULTIPLIED)),
              bitmap.GetAddressOf()))) {
        pendingDecodedIconBytes_ -=
            std::min(pendingDecodedIconBytes_, decoded->second.pixels.size());
        pendingDecodedIcons_.erase(decoded);
        StoreIconBitmap(key, bitmap);
        return bitmap;
      }
      pendingDecodedIconBytes_ -=
          std::min(pendingDecodedIconBytes_, decoded->second.pixels.size());
      pendingDecodedIcons_.erase(decoded);
    }

    QueueIcon(key);
    return nullptr;
  }

  // Hand the key to the persistent icon worker pool. No thread is created here,
  // so repeated searches no longer accumulate threads.
  void QueueIcon(const std::wstring& key) {
    // Start workers only when an icon is actually requested by a visible
    // result.  This keeps idle startup free of icon threads and shell work.
    StartIconWorkers();
    iconResolver_.Queue(key);
  }

  void StartIconWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = std::min<size_t>(3, std::max<unsigned>(2, hw / 2));
    iconResolver_.Start(workers, [this](const std::wstring& key,
                                       std::stop_token stopToken) {
      if (stopThreads_ || stopToken.stop_requested()) {
        return std::optional<feathercast::runtime::DecodedIcon>{};
      }
      return ResolveIcon(key);
    });
  }

  std::optional<feathercast::runtime::DecodedIcon> DecodeIconFile(
      IWICImagingFactory* wicFactory, const std::filesystem::path& path,
      const std::wstring& key) {
    return feathercast::runtime::DecodePngIcon(wicFactory, path, key);
  }

  std::optional<feathercast::runtime::DecodedIcon> ResolveIcon(
      const std::wstring& key) {
    ComPtr<IWICImagingFactory> workerWicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&workerWicFactory)))) {
      return std::nullopt;
    }
    const std::filesystem::path sourcePath(key);
    const std::wstring sourceExtension =
        Lower(sourcePath.extension().wstring());
    std::error_code sourceEc;
    if ((sourceExtension == L".png" || sourceExtension == L".jpg" ||
         sourceExtension == L".jpeg" || sourceExtension == L".bmp") &&
        std::filesystem::is_regular_file(sourcePath, sourceEc)) {
      if (auto decoded = DecodeIconFile(workerWicFactory.Get(), sourcePath, key)) {
        return decoded;
      }
    }
    const auto png = IconCachePath(key);
    std::error_code ec;
    if (!std::filesystem::exists(png, ec)) {
      UniqueBitmap bitmap(CreateShellBitmap(key));
      if (!bitmap) return std::nullopt;
      auto temporary = png;
      temporary += L".tmp-" + std::to_wstring(GetCurrentThreadId());
      std::filesystem::remove(temporary, ec);
      if (!SaveHBitmapPng(workerWicFactory.Get(), bitmap.get(), temporary)) {
        std::filesystem::remove(temporary, ec);
        return std::nullopt;
      }
      if (!MoveFileExW(temporary.c_str(), png.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        if (!std::filesystem::exists(png, ec)) return std::nullopt;
      }
    }
    return DecodeIconFile(workerWicFactory.Get(), png, key);
  }

  HBITMAP CreateShellBitmap(const std::wstring& key) {
    static const std::wstring kAppsFolderPrefix = L"appsFolder:";
    std::wstring parseName = key;
    if (StartsWith(key, kAppsFolderPrefix)) {
      parseName = L"shell:AppsFolder\\" + key.substr(kAppsFolderPrefix.size());
    }

    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(SHCreateItemFromParsingName(parseName.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
      HBITMAP bitmap = nullptr;
      SIZE size{64, 64};
      if (SUCCEEDED(factory->GetImage(size, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmap)) && bitmap) return bitmap;
    }

    if (Lower(key).ends_with(L".lnk")) {
      ShortcutInfo info;
      if (LoadShortcut(key, info)) {
        if (!info.iconPath.empty()) {
          if (HBITMAP bitmap = BitmapFromPathIcon(info.iconPath, info.iconIndex)) return bitmap;
        }
        if (!info.target.empty()) {
          if (HBITMAP bitmap = CreateShellBitmap(info.target)) return bitmap;
        }
      }
    }
    return BitmapFromPathIcon(key, 0);
  }

  HBITMAP BitmapFromPathIcon(const std::wstring& path, int index) {
    HICON rawIcon = nullptr;
    if (!path.empty()) {
      ExtractIconExW(path.c_str(), index, &rawIcon, nullptr, 1);
    }
    if (!rawIcon) {
      SHFILEINFOW info{};
      if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) rawIcon = info.hIcon;
    }
    if (!rawIcon) return nullptr;
    UniqueIcon icon(rawIcon);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    ScreenDC screen;
    // Ownership of the DIB section is handed to the caller as a raw HBITMAP.
    HBITMAP bitmap = CreateDIBSection(screen.get(), &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    UniqueDC dc(CreateCompatibleDC(screen.get()));
    HGDIOBJ old = SelectObject(dc.get(), bitmap);
    DrawIconEx(dc.get(), 0, 0, icon.get(), 64, 64, 0, nullptr, DI_NORMAL);
    SelectObject(dc.get(), old);
    return bitmap;
  }

  bool SaveHBitmapPng(IWICImagingFactory* wicFactory, HBITMAP bitmap,
                      const std::filesystem::path& path) {
    if (!wicFactory || !bitmap) return false;
    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory->CreateBitmapFromHBITMAP(
            bitmap, nullptr, WICBitmapUseAlpha, wicBitmap.GetAddressOf()))) {
      return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                         encoder.GetAddressOf()))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), bag.GetAddressOf()))) return false;
    if (FAILED(frame->Initialize(bag.Get()))) return false;

    UINT w = 0;
    UINT h = 0;
    wicBitmap->GetSize(&w, &h);
    frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&format);
    if (FAILED(frame->WriteSource(wicBitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
  }

  void OnKeyDown(UINT vk) {
    if (overlayClosing_) return;
    if (confirmation_) {
      if (confirmationClosing_) return;
      if (vk == VK_ESCAPE) {
        CancelConfirmation();
      } else if (vk == VK_TAB || vk == VK_LEFT || vk == VK_RIGHT ||
                 vk == VK_UP || vk == VK_DOWN) {
        confirmationFocus_ = 1 - confirmationFocus_;
        InvalidateRect(hwnd_, nullptr, FALSE);
        NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, confirmationFocus_ + 1);
      } else if (vk == VK_RETURN || vk == VK_SPACE) {
        suppressNextChar_ = vk == VK_SPACE;
        ActivateConfirmationChoice();
      }
      return;
    }

    if (vk == VK_ESCAPE) {
      if (browseView_ != BrowseView::None) {
        ExitBrowseView();
      } else if (actionMode_) {
        ExitActionMode();
      } else if (previewOpen_) {
        ClosePreview();
      } else {
        const auto scoped = feathercast::search_scope::Parse(query_);
        if (scoped.recognized) SetQueryText(scoped.terms);
        else HideOverlay(OverlayCloseReason::ExplicitDismiss);
      }
      return;
    }

    if (view_ != View::Search) return;
    const bool control = ModifierPressed(VK_CONTROL);
    const bool shift = ModifierPressed(VK_SHIFT);

    if (control && vk == VK_SPACE) {
      suppressNextChar_ = true;
      TogglePreview();
      return;
    }
    if (previewOpen_ && control &&
        (vk == VK_PRIOR || vk == VK_NEXT)) {
      previewScroll_ = std::max(
          0.0f, previewScroll_ + (vk == VK_NEXT ? 240.0f : -240.0f));
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }

    if (SearchPending()) {
      if (vk == VK_DOWN) {
        pendingNavigation_.Move(1);
        return;
      }
      if (vk == VK_UP) {
        pendingNavigation_.Move(-1);
        return;
      }
      if (vk == VK_PRIOR) {
        pendingNavigation_.Move(-8);
        return;
      }
      if (vk == VK_NEXT) {
        pendingNavigation_.Move(8);
        return;
      }
      if (query_.empty() && vk == VK_HOME) {
        pendingNavigation_.Home();
        return;
      }
      if (query_.empty() && vk == VK_END) {
        pendingNavigation_.End();
        return;
      }
    }

    if (control && vk == 'A') {
      selectionAnchor_ = 0;
      caret_ = query_.size();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    if (control && vk == 'C') {
      CopySelectionToClipboard();
      return;
    }
    if (control && vk == 'X') {
      CopySelectionToClipboard();
      if (DeleteSelection()) {
        feathercast::ui::OverlayController::ResetResultPosition(overlayState_);
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return;
    }
    if (control && vk == 'V') {
      if (const auto text = ReadClipboardText()) {
        InsertQueryText(*text);
        feathercast::ui::OverlayController::ResetResultPosition(overlayState_);
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return;
    }

    if (vk == 'K' && ModifierPressed(VK_CONTROL)) {
      OpenSelectedResultActions();
      return;
    }

    if (vk == VK_DOWN) {
      SelectResult(selected_ + 1, true, true);
    } else if (vk == VK_UP) {
      SelectResult(selected_ - 1, true, true);
    } else if (vk == VK_HOME) {
      if (!query_.empty()) {
        MoveCaret(0, shift);
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else {
        SelectResult(0, true, true);
      }
    } else if (vk == VK_END) {
      if (!query_.empty()) {
        MoveCaret(query_.size(), shift);
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else {
        SelectResult(static_cast<int>(flatItems_.size()) - 1, true, true);
      }
    } else if (vk == VK_PRIOR) {
      SelectResult(selected_ - 8, true, true);
    } else if (vk == VK_NEXT) {
      SelectResult(selected_ + 8, true, true);
    } else if (vk == VK_RIGHT) {
      if (caret_ < query_.size()) {
        MoveCaret(control
                      ? feathercast::text_edit::NextWord(query_, caret_)
                      : feathercast::text_edit::NextCodePoint(query_, caret_),
                  shift);
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else if (query_.empty() && browseView_ == BrowseView::None &&
                 ResultsActivationAllowed()) {
        OpenSelectedResultActions();
      }
    } else if (vk == VK_TAB) {
      if (selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size()) &&
          flatItems_[static_cast<std::size_t>(selected_)].isCapability &&
          flatItems_[static_cast<std::size_t>(selected_)].capability.stableId
              .starts_with(L"scope:")) {
        ActivateResultAt(selected_, false);
      } else {
        OpenSelectedResultActions();
      }
    } else if (vk == VK_LEFT) {
      if (caret_ > 0) {
        MoveCaret(control
                      ? feathercast::text_edit::PreviousWord(query_, caret_)
                      : feathercast::text_edit::PreviousCodePoint(query_, caret_),
                  shift);
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else if (browseView_ != BrowseView::None) {
        ExitBrowseView();
      } else if (actionMode_) {
        ExitActionMode();
      }
    } else if (vk == VK_RETURN) {
      const bool admin = ModifierPressed(VK_CONTROL) && ModifierPressed(VK_SHIFT);
      ActivateResultAt(selected_, admin);
    } else if (vk == VK_BACK) {
      if (!query_.empty() && (caret_ > 0 || SelectionRange())) {
        if (DeleteSelection()) {
          // Selection deletion already updated the caret.
        } else if (control) {
          const size_t previous = feathercast::text_edit::PreviousWord(query_, caret_);
          query_.erase(previous, caret_ - previous);
          caret_ = previous;
        } else {
          feathercast::text_edit::ErasePrevious(query_, caret_);
        }
        feathercast::ui::OverlayController::ResetResultPosition(overlayState_);
        SyncSelectionAnimationToTarget();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
    } else if (vk == VK_DELETE) {
      if (!query_.empty() && (caret_ < query_.size() || SelectionRange())) {
        if (DeleteSelection()) {
          // Selection deletion already updated the caret.
        } else if (control) {
          query_.erase(caret_, feathercast::text_edit::NextWord(query_, caret_) - caret_);
        } else {
          feathercast::text_edit::EraseNext(query_, caret_);
        }
        feathercast::ui::OverlayController::ResetResultPosition(overlayState_);
        SyncSelectionAnimationToTarget();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
    }
  }

  void OnChar(wchar_t ch) {
    if (suppressNextChar_) {
      suppressNextChar_ = false;
      return;
    }
    if (confirmation_) return;
    if (view_ != View::Search) return;
    if (ch >= 32 && ch != 127) {
      ClampCaret();
      InsertQueryText(std::wstring(1, ch));
      SyncSelectionAnimationToTarget();
      RequestSearch();
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

  void EnsureSelectedVisible() {
    int y = 0;
    int row = 0;
    for (const auto& section : sections_) {
      y += static_cast<int>(kSectionHeaderHeight);
      for (size_t i = 0; i < section.items.size(); ++i) {
        if (row == selected_) {
          const int rowTop = y;
          const int rowBottom = y + static_cast<int>(kResultRowHeight);
          RECT rc{};
          GetClientRect(hwnd_, &rc);
          const float scale = GetWindowScale(hwnd_);
          const int visible = static_cast<int>((rc.bottom - rc.top) / scale) - static_cast<int>(kResultsTop) - (settings_.compactMode ? 0 : 36);
          int nextScroll = scroll_;
          if (rowTop - scroll_ < 0) {
            nextScroll = rowTop;
          } else if (rowBottom - scroll_ > visible) {
            nextScroll = rowBottom - visible;
          }
          feathercast::ui::OverlayController::SetScroll(
              overlayState_, nextScroll,
              std::max(0, ResultsContentHeight() - visible));
          RetargetOverlayScroll();
          return;
        }
        y += static_cast<int>(kResultRowStride);
        ++row;
      }
    }
  }

  void OnMouseMove(float x, float y) {
    UpdatePointerPress(hwnd_, x, y);
    if (!mouseTracking_) {
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&tme);
      mouseTracking_ = true;
    }

    // Suppress hover until the cursor leaves the spot it occupied when the
    // overlay opened; a window appearing under the pointer fires WM_MOUSEMOVE
    // even though the user never moved the mouse.
    if (ignoreMouseUntilMove_) {
      POINT cursor{};
      GetCursorPos(&cursor);
      if (cursor.x == mouseAnchor_.x && cursor.y == mouseAnchor_.y) return;
      ignoreMouseUntilMove_ = false;
    }

    if (confirmation_) {
      int hover = -1;
      for (const auto& hit : hits_) {
        if (PointInRect(hit.rect, x, y)) {
          if (hit.type == HitType::ConfirmCancel) hover = 0;
          else if (hit.type == HitType::ConfirmAction) hover = 1;
          break;
        }
      }
      if (hover != confirmationHover_) {
        confirmationHover_ = hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return;
    }

    bool gearNow = false;
    for (const auto& hit : hits_) {
      if (hit.type == HitType::Gear && PointInRect(hit.rect, x, y)) {
        gearNow = true;
        break;
      }
    }
    if (gearNow != gearHovered_) {
      gearHovered_ = gearNow;
      InvalidateRect(hwnd_, nullptr, FALSE);
    }

    for (const auto& hit : hits_) {
      if (hit.type == HitType::Result && PointInRect(hit.rect, x, y) && hit.index != selected_) {
        SelectResult(hit.index, true, false, SelectionMotion::Hover);
        return;
      }
    }
  }

  void OnSettingsMouseMove(float x, float y) {
    if (pointerPress_ && pointerPress_->owner == settingsHwnd_ &&
        pointerPress_->type == HitType::AnimationLevel &&
        GetCapture() == settingsHwnd_) {
      SetAnimationLevelFromSliderX(x);
    }
    UpdatePointerPress(settingsHwnd_, x, y);
    if (!mouseTracking_) {
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, settingsHwnd_, 0};
      TrackMouseEvent(&tme);
      mouseTracking_ = true;
    }
    int hover = -1;
    for (const auto& hit : hits_) {
      if (PointInRect(hit.rect, x, y)) {
        hover = static_cast<int>(hit.type);
        break;
      }
    }
    if (hover != settingsHover_) {
      settingsHover_ = hover;
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    }
  }

  void SelectSettingsCategory(SettingsCategory category) {
    const bool changed = settingsCategory_ != category;
    const bool wasRecording = recording_;
    feathercast::ui::SettingsController::SelectCategory(settingsState_, category);
    settingsFocusIndex_ = SettingsCategoryIndex(category);
    if (category == SettingsCategory::Privacy && settings_.fileIndexEnabled) {
      EnsureFileIndexLoaded();
      if (!fileIndexConfigured_) ConfigureFileIndex();
    }
    if (wasRecording) {
      shortcutRecorder_.Reset();
    }
    if (changed) {
      settingsPageProgress_.Snap(0.0);
      settingsPageProgress_.Retarget(1.0, 0.120, FadeAnimationsAllowed());
      settingsCategoryTop_.Retarget(
          kSettTop + static_cast<double>(SettingsCategoryIndex(category)) *
                         kSettCategoryRow,
          0.090, SpatialAnimationsAllowed());
      RetargetSettingsScroll();
      ResizeSettingsWindow();
      RequestAnimationFrame();
    }
    if (settingsHwnd_) {
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_FOCUS, settingsHwnd_, OBJID_CLIENT,
                     settingsFocusIndex_ + 1);
    }
  }

  void NotifyAnimationLevelChanged() {
    if (!settingsHwnd_) return;
    const auto order = SettingsFocusOrder();
    const auto item = std::find(order.begin(), order.end(),
                                HitType::AnimationLevel);
    if (item == order.end()) return;
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, settingsHwnd_, OBJID_CLIENT,
                   static_cast<LONG>(std::distance(order.begin(), item)) + 1);
  }

  void SetAnimationLevel(AnimationLevel level) {
    if (settings_.animationLevel == level) return;
    settings_.animationLevel = level;
    if (level == AnimationLevel::Off) {
      SnapAllMotionToTargets();
    } else if (level == AnimationLevel::Reduced) {
      SnapSpatialMotionToTargets();
    }
    PersistSettings();
    if (settingsHwnd_) InvalidateRect(settingsHwnd_, nullptr, FALSE);
    NotifyAnimationLevelChanged();
  }

  void SetAnimationLevelFromSliderX(float x) {
    const auto hit = std::find_if(
        hits_.begin(), hits_.end(), [](const HitTarget& target) {
          return target.type == HitType::AnimationLevel;
        });
    if (hit == hits_.end()) return;
    const RectF track = AnimationSliderTrackRect(hit->rect);
    const float progress = std::clamp(
        (x - track.left) / std::max(1.0f, track.right - track.left), 0.0f,
        1.0f);
    const int index = static_cast<int>(std::lround(progress * 2.0f));
    SetAnimationLevel(static_cast<AnimationLevel>(index));
  }

  bool PointerPressMatchesHit(const PointerPress& press, const HitTarget& hit,
                              float x, float y) const {
    if (hit.type != press.type || hit.index != press.index ||
        !PointInRect(hit.rect, x, y)) {
      return false;
    }
    if (press.type != HitType::Result) return true;
    if (press.index < 0 ||
        press.index >= static_cast<int>(flatItems_.size())) {
      return false;
    }
    return feathercast::interaction::StablePointerTargetMatches(
        static_cast<int>(press.type), press.index, press.searchGeneration,
        press.targetKey, static_cast<int>(hit.type), hit.index,
        searchPresentation_.Displayed(),
        flatItems_[static_cast<size_t>(press.index)].Key(), true,
        ResultsActivationAllowed() &&
            press.searchGeneration == searchPresentation_.Requested());
  }

  bool PointerPressMatchesAt(const PointerPress& press, float x, float y) const {
    return std::any_of(hits_.begin(), hits_.end(), [&](const HitTarget& hit) {
      return PointerPressMatchesHit(press, hit, x, y);
    });
  }

  void CancelPointerPress(HWND owner = nullptr) {
    if (!pointerPress_ || (owner && pointerPress_->owner != owner)) return;
    const HWND captureOwner = pointerPress_->owner;
    pointerPress_.reset();
    if (captureOwner && GetCapture() == captureOwner) ReleaseCapture();
    if (captureOwner && IsWindow(captureOwner)) {
      InvalidateRect(captureOwner, nullptr, FALSE);
    }
  }

  void UpdatePointerPress(HWND owner, float x, float y) {
    if (!pointerPress_ || pointerPress_->owner != owner) return;
    const bool inside = PointerPressMatchesAt(*pointerPress_, x, y);
    if (inside != pointerPress_->inside) {
      pointerPress_->inside = inside;
      InvalidateRect(owner, nullptr, FALSE);
    }
  }

  void OnPointerDown(HWND owner, float x, float y) {
    CancelPointerPress();

    if (owner == hwnd_ && !confirmation_ && view_ == View::Search) {
      RECT rc{};
      GetClientRect(hwnd_, &rc);
      const float scale = GetWindowScale(hwnd_);
      const float width = static_cast<float>(rc.right - rc.left) / scale;
      if (PointInRect({0, 0, width - 60.0f, 60.0f}, x, y)) {
        SetCaretFromSearchClick(x);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }
    }

    const auto hit = std::find_if(hits_.begin(), hits_.end(),
                                  [&](const HitTarget& target) {
                                    return PointInRect(target.rect, x, y);
                                  });
    if (hit == hits_.end()) return;
    if (owner == settingsHwnd_ && !SettingsControlEnabled(hit->type)) return;
    if (hit->type == HitType::Result && !ResultsActivationAllowed()) return;

    PointerPress press;
    press.owner = owner;
    press.type = hit->type;
    press.index = hit->index;
    press.inside = true;
    if (hit->type == HitType::Result) {
      if (hit->index < 0 || hit->index >= static_cast<int>(flatItems_.size())) {
        return;
      }
      press.searchGeneration = searchPresentation_.Displayed();
      press.targetKey = flatItems_[static_cast<size_t>(hit->index)].Key();
    }
    pointerPress_ = std::move(press);

    if (owner == settingsHwnd_) {
      const auto order = SettingsFocusOrder();
      const auto focused = std::find(order.begin(), order.end(), hit->type);
      if (focused != order.end()) {
        settingsFocusIndex_ =
            static_cast<int>(std::distance(order.begin(), focused));
      }
      if (hit->type == HitType::AnimationLevel) {
        SetAnimationLevelFromSliderX(x);
      }
    } else if (hit->type == HitType::ConfirmCancel) {
      confirmationFocus_ = 0;
    } else if (hit->type == HitType::ConfirmAction) {
      confirmationFocus_ = 1;
    }

    SetCapture(owner);
    InvalidateRect(owner, nullptr, FALSE);
  }

  void OnPointerUp(HWND owner, float x, float y) {
    if (!pointerPress_ || pointerPress_->owner != owner) return;
    PointerPress press = std::move(*pointerPress_);
    const bool animationSliderDrag =
        owner == settingsHwnd_ && press.type == HitType::AnimationLevel;
    const bool activate = PointerPressMatchesAt(press, x, y);
    pointerPress_.reset();
    if (GetCapture() == owner) ReleaseCapture();
    InvalidateRect(owner, nullptr, FALSE);
    if (animationSliderDrag) {
      SetAnimationLevelFromSliderX(x);
      return;
    }
    if (!activate) return;

    if (owner == settingsHwnd_) {
      if (press.type == HitType::CloseSettings) {
        HideSettings();
      } else {
        HandleSettingsHit(press.type);
      }
      return;
    }

    switch (press.type) {
      case HitType::Result:
        ActivateResultAt(press.index, false, press.searchGeneration,
                         press.targetKey);
        break;
      case HitType::Gear:
        OpenSettings();
        break;
      case HitType::ConfirmCancel:
        CancelConfirmation();
        break;
      case HitType::ConfirmAction:
        confirmationFocus_ = 1;
        ActivateConfirmationChoice();
        break;
      default:
        break;
    }
  }

  void OnRightClick(float x, float y) {
    if (!ResultsActivationAllowed() || actionMode_ ||
        browseView_ != BrowseView::None) {
      return;
    }
    for (const auto& hit : hits_) {
      if (hit.type != HitType::Result || !PointInRect(hit.rect, x, y)) continue;
      if (hit.index >= 0 && hit.index < static_cast<int>(flatItems_.size())) {
        SelectResult(hit.index, false, true);
        EnterActionMode(flatItems_[hit.index]);
      }
      return;
    }
  }

  void OpenSelectedResultActions() {
    if (!ResultsActivationAllowed() || actionMode_ ||
        browseView_ != BrowseView::None ||
        selected_ < 0 || selected_ >= static_cast<int>(flatItems_.size())) {
      return;
    }
    EnterActionMode(flatItems_[static_cast<size_t>(selected_)]);
  }

  void ResizeSettingsWindow() {
    if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) return;
    RECT rc{};
    GetWindowRect(settingsHwnd_, &rc);
    const float scale = GetWindowScale(settingsHwnd_);
    const int width = rc.right - rc.left;
    HMONITOR monitor = MonitorFromWindow(settingsHwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const int desiredHeight = static_cast<int>(SETTINGS_HEIGHT * scale);
    const int maxHeight =
        mi.rcWork.bottom - mi.rcWork.top - static_cast<int>(40 * scale);
    const int physicalHeight = std::min(desiredHeight, maxHeight);
    const int centerY = rc.top + (rc.bottom - rc.top) / 2;
    const int minTop = mi.rcWork.top + static_cast<int>(20 * scale);
    const int maxTop =
        mi.rcWork.bottom - static_cast<int>(20 * scale) - physicalHeight;
    const int top = std::clamp(centerY - physicalHeight / 2, minTop,
                               std::max(minTop, maxTop));
    if (SpatialAnimationsAllowed()) {
      if (!settingsBounds_.Active()) SnapWindowBounds(settingsBounds_, rc);
      settingsBounds_.Retarget(rc.left, top, width, physicalHeight,
                               kSettingsResizeSeconds, true);
      RequestAnimationFrame();
    } else {
      SetWindowPos(settingsHwnd_, HWND_NOTOPMOST, rc.left, top, width,
                   physicalHeight, SWP_NOACTIVATE);
      RECT applied{};
      GetWindowRect(settingsHwnd_, &applied);
      SnapWindowBounds(settingsBounds_, applied);
    }
    // Visible corners are drawn by Direct2D; no window region, so DirectComposition alpha stays intact.
  }

  void EnsureSettingsFocusVisible() {
    const auto order = SettingsFocusOrder();
    if (settingsFocusIndex_ < 0 || settingsFocusIndex_ >= static_cast<int>(order.size())) return;
    const HitType focused = order[static_cast<size_t>(settingsFocusIndex_)];
    RECT client{};
    GetClientRect(settingsHwnd_, &client);
    const float height = static_cast<float>(client.bottom - client.top) / GetWindowScale(settingsHwnd_);
    for (const auto& hit : hits_) {
      if (hit.type != focused) continue;
      float nextScroll = settingsScroll_;
      if (hit.rect.top < 64.0f) {
        nextScroll -= 64.0f - hit.rect.top;
      } else if (hit.rect.bottom > height - 8.0f) {
        nextScroll += hit.rect.bottom - (height - 8.0f);
      }
      feathercast::ui::SettingsController::SetScroll(
          settingsState_, nextScroll, SettingsMaxScroll());
      RetargetSettingsScroll();
      return;
    }
  }

  void HandleSettingsKeyDown(UINT vk) {
    if (recording_) return;
    const auto order = SettingsFocusOrder();
    if (order.empty()) return;
    settingsFocusIndex_ = std::clamp(
        settingsFocusIndex_, 0, static_cast<int>(order.size()) - 1);
    const HitType focusedType = order[static_cast<size_t>(settingsFocusIndex_)];
    const auto focusedCategory = SettingsCategoryForHit(focusedType);

    if (focusedCategory && (vk == VK_UP || vk == VK_DOWN)) {
      const int direction = vk == VK_UP ? -1 : 1;
      const int current = SettingsCategoryIndex(*focusedCategory);
      const int count = static_cast<int>(kSettingsCategories.size());
      const int next = (current + direction + count) % count;
      SelectSettingsCategory(kSettingsCategories[static_cast<size_t>(next)]);
      return;
    }
    if (focusedType == HitType::AnimationLevel) {
      if (vk == VK_LEFT || vk == VK_DOWN) {
        SetAnimationLevel(feathercast::settings::StepAnimationLevel(
            settings_.animationLevel, -1));
        return;
      }
      if (vk == VK_RIGHT || vk == VK_UP) {
        SetAnimationLevel(feathercast::settings::StepAnimationLevel(
            settings_.animationLevel, 1));
        return;
      }
      if (vk == VK_HOME) {
        SetAnimationLevel(AnimationLevel::Off);
        return;
      }
      if (vk == VK_END) {
        SetAnimationLevel(AnimationLevel::Full);
        return;
      }
    }
    if (vk == VK_LEFT && !focusedCategory) {
      feathercast::ui::SettingsController::SetFocus(
          settingsState_, SettingsCategoryIndex(settingsCategory_),
          static_cast<int>(order.size()));
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_FOCUS, settingsHwnd_, OBJID_CLIENT,
                     settingsFocusIndex_ + 1);
      return;
    }
    if (vk == VK_RIGHT && focusedCategory &&
        order.size() > kSettingsCategories.size() + 1) {
      feathercast::ui::SettingsController::SetFocus(
          settingsState_, static_cast<int>(kSettingsCategories.size()),
          static_cast<int>(order.size()));
      EnsureSettingsFocusVisible();
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_FOCUS, settingsHwnd_, OBJID_CLIENT,
                     settingsFocusIndex_ + 1);
      return;
    }
    if (vk == VK_TAB) {
      const int direction = ModifierPressed(VK_SHIFT) ? -1 : 1;
      feathercast::ui::SettingsController::CycleFocus(
          settingsState_, direction, static_cast<int>(order.size()));
      EnsureSettingsFocusVisible();
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_FOCUS, settingsHwnd_, OBJID_CLIENT, settingsFocusIndex_ + 1);
      return;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
      if (focusedType == HitType::CloseSettings) HideSettings();
      else HandleSettingsHit(focusedType);
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    }
  }

  std::optional<std::wstring> PickIndexedFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
      return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose a folder for FeatherCast to index");
    if (FAILED(dialog->Show(settingsHwnd_))) return std::nullopt;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
      return std::nullopt;
    }
    CoMemPtr<wchar_t> pathOwner(rawPath);
    return std::wstring(rawPath);
  }

  void HandleSettingsHit(HitType type) {
    if (!SettingsControlEnabled(type)) return;
    switch (type) {
      case HitType::SettingsShortcutCategory:
      case HitType::SettingsGeneralCategory:
      case HitType::SettingsResultsCategory:
      case HitType::SettingsLibraryCategory:
      case HitType::SettingsPrivacyCategory:
      case HitType::SettingsExtensionsCategory:
      case HitType::SettingsAppearanceCategory:
      case HitType::SettingsMaintenanceCategory:
        if (const auto category = SettingsCategoryForHit(type)) {
          SelectSettingsCategory(*category);
        }
        break;
      case HitType::ManageSnippets:
        OpenLibraryManager(feathercast::library::ItemKind::Snippet);
        break;
      case HitType::ManageQuicklinks:
        OpenLibraryManager(feathercast::library::ItemKind::Quicklink);
        break;
      case HitType::RecordShortcut:
        shortcutRecorder_.Reset();
        feathercast::ui::SettingsController::BeginShortcutRecording(
            settingsState_);
        break;
      case HitType::SaveShortcut:
        if (!pendingShortcut_.empty()) {
          if (pendingShortcut_ == settings_.shortcut) {
            pendingShortcut_.clear();
            break;
          }
          const ShortcutSpec candidate = ParseShortcut(pendingShortcut_);
          if (!CanActivateShortcut(candidate)) {
            MessageBoxW(
                settingsHwnd_,
                L"That shortcut is already reserved by Windows or another application. "
                L"The current FeatherCast shortcut was kept.",
                L"FeatherCast Shortcut",
                MB_OK | MB_ICONWARNING);
            break;
          }
          const std::wstring previousText = settings_.shortcut;
          const ShortcutSpec previousShortcut = shortcut_;
          settings_.shortcut = pendingShortcut_;
          shortcut_ = candidate;
          shortcutRuntime_ = ShortcutRuntime{};
          const bool hotKeyReady = RegisterShortcutHotKey();
          if (!hotKeyReady && !hook_) {
            settings_.shortcut = previousText;
            shortcut_ = previousShortcut;
            RegisterShortcutHotKey();
            MessageBoxW(settingsHwnd_,
                        L"FeatherCast could not activate that shortcut. The previous shortcut was restored.",
                        L"FeatherCast Shortcut", MB_OK | MB_ICONWARNING);
            break;
          }
          pendingShortcut_.clear();
          PersistSettings();
        }
        break;
      case HitType::ClearShortcut:
        settings_.shortcut = L"none";
        shortcut_ = ParseShortcut(settings_.shortcut);
        shortcutRuntime_ = ShortcutRuntime{};
        UnregisterShortcutHotKey();
        pendingShortcut_.clear();
        PersistSettings();
        break;
      case HitType::StartupToggle:
        if (const bool desired = !settings_.startOnStartup; SetStartOnStartup(desired)) {
          settings_.startOnStartup = desired;
          PersistSettings();
        } else {
          MessageBoxW(settingsHwnd_, L"Windows rejected the startup setting change.",
                      L"FeatherCast Settings", MB_OK | MB_ICONWARNING);
        }
        break;
      case HitType::UpdateChecksToggle:
        settings_.updateChecksEnabled = !settings_.updateChecksEnabled;
        if (settings_.updateChecksEnabled) {
          settings_.lastUpdateAttempt = 0;
          settings_.lastUpdateCheck = 0;
        }
        PersistSettings();
        break;
      case HitType::ShowWindowsToggle:
        settings_.showOpenWindows = !settings_.showOpenWindows;
        PersistSettings();
        RefreshWindowsAsync();
        RequestSearch();
        break;
      case HitType::ShowStoreAppsToggle:
        settings_.showStoreApps = !settings_.showStoreApps;
        PersistSettings();
        RequestSearch();
        break;
      case HitType::ClipboardHistoryToggle:
        settings_.privacyConsentVersion = 1;
        settings_.clipboardHistoryEnabled = !settings_.clipboardHistoryEnabled;
        if (settings_.clipboardHistoryEnabled) {
          if (!persistence_.LoadClipboard(ClipboardHistoryLimit())) {
            ReportPersistenceFailure(
                L"The persistence worker is unavailable.");
          }
        } else {
          {
            std::lock_guard lock(dataMutex_);
            clipboardHistory_.clear();
          }
          if (browseView_ == BrowseView::Clipboard) ExitBrowseView();
        }
        UpdateClipboardListenerRegistration();
        PersistSettings();
        RequestSearch();
        break;
      case HitType::ClipboardLimitDown:
        settings_.clipboardHistoryLimit = std::clamp(
            settings_.clipboardHistoryLimit - 10,
            MIN_CLIPBOARD_HISTORY_LIMIT, MAX_CLIPBOARD_HISTORY_LIMIT);
        PersistSettings();
        ApplyClipboardHistoryLimit();
        break;
      case HitType::ClipboardLimitUp:
        settings_.clipboardHistoryLimit = std::clamp(
            settings_.clipboardHistoryLimit + 10,
            MIN_CLIPBOARD_HISTORY_LIMIT, MAX_CLIPBOARD_HISTORY_LIMIT);
        PersistSettings();
        ApplyClipboardHistoryLimit();
        break;
      case HitType::FileIndexToggle:
        settings_.privacyConsentVersion = 1;
        settings_.fileIndexEnabled = !settings_.fileIndexEnabled;
        if (!settings_.fileIndexEnabled) {
          settings_.fileContentIndexEnabled = false;
          StopFileIndexService();
          std::lock_guard lock(dataMutex_);
          fileIndex_.clear();
          fileIndexLoaded_ = false;
        }
        PersistSettings();
        if (settings_.fileIndexEnabled) {
          EnsureFileIndexLoaded();
          ConfigureFileIndex();
        }
        RequestSearch();
        break;
      case HitType::FileContentIndexToggle:
        if (!settings_.fileContentIndexEnabled) {
          const int answer = MessageBoxW(
              settingsHwnd_,
              L"FeatherCast will read supported text files only inside your "
              L"selected local folders. It stores a local searchable token "
              L"index, not the original text or excerpts. The index can still "
              L"reveal words present in those files. Files are limited to 2 MiB "
              L"and content indexing can be disabled or deleted at any time.",
              L"Enable Search File Contents?",
              MB_ICONINFORMATION | MB_OKCANCEL | MB_DEFBUTTON2);
          if (answer != IDOK) break;
        }
        settings_.privacyConsentVersion = 1;
        settings_.fileContentIndexEnabled =
            !settings_.fileContentIndexEnabled;
        PersistSettings();
        if (settings_.fileIndexEnabled) ConfigureFileIndex();
        break;
      case HitType::FileIndexLimitDown:
        settings_.fileIndexMaxEntries = std::clamp(
            settings_.fileIndexMaxEntries - 1000,
            MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
        PersistSettings();
        if (settings_.fileIndexEnabled) ConfigureFileIndex();
        break;
      case HitType::FileIndexLimitUp:
        settings_.fileIndexMaxEntries = std::clamp(
            settings_.fileIndexMaxEntries + 1000,
            MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
        PersistSettings();
        if (settings_.fileIndexEnabled) ConfigureFileIndex();
        break;
      case HitType::AddFileRoot:
        if (const auto folder = PickIndexedFolder()) {
          if (settings_.fileIndexRoots.empty()) {
            settings_.fileIndexRoots = ResolvedFileIndexRoots();
          }
          const std::wstring normalized = Lower(*folder);
          const bool exists = std::any_of(
              settings_.fileIndexRoots.begin(), settings_.fileIndexRoots.end(),
              [&](const std::wstring& root) { return Lower(root) == normalized; });
          if (!exists) settings_.fileIndexRoots.push_back(*folder);
          PersistSettings();
          if (settings_.fileIndexEnabled) ConfigureFileIndex();
          ResizeSettingsWindow();
        }
        break;
      case HitType::RemoveFileRoot:
        {
          const auto roots = ResolvedFileIndexRoots();
          if (roots.size() <= 1) {
            SetSettingsStatus(
                StatusSeverity::Info,
                L"At least one indexed folder is required. Add another folder first.");
            break;
          }
          HMENU menu = CreatePopupMenu();
          if (!menu) break;
          for (std::size_t index = 0; index < roots.size(); ++index) {
            const std::filesystem::path root(roots[index]);
            std::wstring label = root.filename().wstring();
            if (label.empty()) label = roots[index];
            AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(index + 1),
                        label.c_str());
          }
          RECT settingsRect{};
          GetWindowRect(settingsHwnd_, &settingsRect);
          const UINT selected = TrackPopupMenu(
              menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
              settingsRect.left + 260, settingsRect.top + 180, 0,
              settingsHwnd_, nullptr);
          DestroyMenu(menu);
          if (selected == 0 || selected > roots.size()) break;
          settings_.fileIndexRoots = roots;
          settings_.fileIndexRoots.erase(settings_.fileIndexRoots.begin() +
                                         static_cast<std::ptrdiff_t>(selected - 1));
          PersistSettings();
          if (settings_.fileIndexEnabled) ConfigureFileIndex();
          ResizeSettingsWindow();
        }
        break;
      case HitType::ClearFileRoots:
        settings_.fileIndexRoots.clear();
        PersistSettings();
        if (settings_.fileIndexEnabled) ConfigureFileIndex();
        break;
      case HitType::RebuildFileIndex:
        SetSettingsStatus(StatusSeverity::Info, L"Rebuilding the file index...");
        if (!fileIndexService_.Rebuild()) {
          ReportBackgroundFailure(L"The live file index worker is unavailable.");
        }
        break;
      case HitType::DiagnosticsToggle:
        settings_.diagnosticsEnabled = !settings_.diagnosticsEnabled;
        g_diagnosticsEnabled.store(settings_.diagnosticsEnabled,
                                   std::memory_order_release);
        PersistSettings();
        break;
      case HitType::ClearClipboardData:
        ClearClipboardHistoryData();
        break;
      case HitType::ClearFileIndexData:
        ClearFileIndexData();
        break;
      case HitType::OpenLocalDataFolder:
        launchExecutor_.Submit([path = LocalDataPath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
          CoUninitialize();
        });
        break;
      case HitType::ReloadExtensions:
        extensionReloadPending_ = true;
        SetSettingsStatus(StatusSeverity::Progress, L"Reloading extensions...");
        if (!launchExecutor_.Submit([this](std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          extensions_.Reload();
          if (!stopToken.stop_requested() && !stopThreads_) {
            PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
            PostMessageW(hwnd_, WM_EXTENSION_RELOAD_READY, 0, 0);
          }
        })) {
          extensionReloadPending_ = false;
          SetSettingsStatus(StatusSeverity::Error,
                            L"The extension worker is unavailable.");
        }
        break;
      case HitType::OpenPluginsFolder:
        launchExecutor_.Submit([path = UserDataPath() / L"plugins"](std::stop_token) {
          std::error_code ec;
          std::filesystem::create_directories(path, ec);
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
          CoUninitialize();
        });
        break;
      case HitType::ClearRecents:
        settings_.recentApps.clear();
        settings_.usageStats.clear();
        PersistSettings();
        RequestSearch();
        break;
      case HitType::ClearIconCache:
        ClearIconCache();
        break;
      case HitType::CheckUpdates:
        StartUpdateCheck(true);
        break;
      case HitType::OverlayWidthDown:
        settings_.overlayWidth = std::clamp(OverlayWidth() - 40, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
        PersistSettings();
        ApplyWindowSize();
        break;
      case HitType::OverlayWidthUp:
        settings_.overlayWidth = std::clamp(OverlayWidth() + 40, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
        PersistSettings();
        ApplyWindowSize();
        break;
      case HitType::MaxResultsDown:
        settings_.maxResults = std::clamp(settings_.maxResults - 25, MIN_RESULTS, MAX_RESULT_SETTING);
        PersistSettings();
        RequestSearch();
        break;
      case HitType::MaxResultsUp:
        settings_.maxResults = std::clamp(settings_.maxResults + 25, MIN_RESULTS, MAX_RESULT_SETTING);
        PersistSettings();
        RequestSearch();
        break;
      case HitType::CompactToggle:
        settings_.compactMode = !settings_.compactMode;
        PersistSettings();
        break;
      case HitType::AnimationLevel: {
        const auto next = settings_.animationLevel == AnimationLevel::Full
                              ? AnimationLevel::Off
                              : feathercast::settings::StepAnimationLevel(
                                    settings_.animationLevel, 1);
        SetAnimationLevel(next);
        break;
      }
      case HitType::AccentToggle:
        settings_.syncAccentColor = !settings_.syncAccentColor;
        PersistSettings();
        ResizeSettingsWindow();
        break;
      case HitType::AccentColor:
        ChooseAccentColor();
        ResizeSettingsWindow();
        break;
      default:
        break;
    }
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void OnMouseWheel(int delta) {
    if (view_ != View::Search) return;
    if (previewOpen_) {
      RECT client{};
      GetClientRect(hwnd_, &client);
      POINT cursor{};
      GetCursorPos(&cursor);
      ScreenToClient(hwnd_, &cursor);
      const float scale = GetWindowScale(hwnd_);
      const float x = static_cast<float>(cursor.x) / scale;
      const float y = static_cast<float>(cursor.y) / scale;
      const float width = static_cast<float>(client.right) / scale;
      const float baseWidth = static_cast<float>(std::clamp(
          settings_.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH));
      const bool sideBySide = width >= baseWidth + 340.0f;
      if (y >= kResultsTop && (!sideBySide || x >= baseWidth)) {
        previewScroll_ = std::max(
            0.0f, previewScroll_ -
                      static_cast<float>(feathercast::motion::WheelDeltaPixels(delta)));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }
    }
    if (sections_.empty()) return;
    const int previousScroll = scroll_;
    const int nextScroll = static_cast<int>(std::lround(
        static_cast<double>(scroll_) -
        feathercast::motion::WheelDeltaPixels(delta)));
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetWindowScale(hwnd_);
    const int visible = static_cast<int>((rc.bottom - rc.top) / scale) - static_cast<int>(kResultsTop) - (settings_.compactMode ? 0 : 36);
    feathercast::ui::OverlayController::SetScroll(
        overlayState_, nextScroll,
        std::max(0, ResultsContentHeight() - visible));
    if (scroll_ != previousScroll) RetargetOverlayScroll();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // Largest scroll offset (logical px) so the bottom of the settings content
  // stops flush with the window's bottom edge. Zero when everything fits.
  float SettingsMaxScroll() const {
    if (!settingsHwnd_) return 0.0f;
    RECT rc{};
    GetClientRect(settingsHwnd_, &rc);
    const float scale = GetWindowScale(settingsHwnd_);
    const float viewHeight = static_cast<float>(rc.bottom - rc.top) / scale;
    return std::max(
        0.0f, static_cast<float>(SettingsPageContentHeight()) - viewHeight);
  }

  void OnSettingsMouseWheel(int delta) {
    const float maxScroll = SettingsMaxScroll();
    if (maxScroll <= 0.0f) return;
    feathercast::ui::SettingsController::ScrollBy(
        settingsState_,
        -static_cast<float>(
            feathercast::motion::WheelDeltaPixels(delta, 60.0)),
        maxScroll);
    RetargetSettingsScroll();
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void ChooseAccentColor() {
    COLORREF custom[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = settingsHwnd_;
    cc.rgbResult = ColorRefFromHex(settings_.customAccentColor);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    suppressHide_ = true;
    if (ChooseColorW(&cc)) {
      settings_.customAccentColor = HexFromColorRef(cc.rgbResult);
      settings_.syncAccentColor = false;
      PersistSettings();
    }
    suppressHide_ = false;
    SetForegroundWindow(settingsHwnd_);
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void ActivateExtension(const DisplayItem& item) {
    if (extensionActivationPending_) return;
    extensionActivationPending_ = true;
    const auto extensionItem = item.extension;
    if (!launchExecutor_.Submit([this, extensionItem](std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          ExtensionActivationResult result;
          result.response = extensions_.Activate(extensionItem);
          if (stopToken.stop_requested() || stopThreads_) return;
          {
            std::lock_guard lock(extensionActivationMutex_);
            latestExtensionActivation_ = std::move(result);
          }
          PostMessageW(hwnd_, WM_EXTENSION_ACTIVATED, 0, 0);
        })) {
      extensionActivationPending_ = false;
    }
  }

  void OnImeComposition(LPARAM flags) {
    HIMC context = ImmGetContext(hwnd_);
    if (!context) return;
    ScopeExit releaseContext([&] { ImmReleaseContext(hwnd_, context); });

    auto readComposition = [&](DWORD kind) {
      const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
      if (bytes <= 0) return std::wstring{};
      std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
      ImmGetCompositionStringW(context, kind, text.data(), bytes);
      return text;
    };

    if ((flags & GCS_RESULTSTR) != 0) {
      const std::wstring result = readComposition(GCS_RESULTSTR);
      imeComposition_.clear();
      if (!result.empty()) {
        InsertQueryText(result);
        feathercast::ui::OverlayController::ResetResultPosition(overlayState_);
        SyncSelectionAnimationToTarget();
        RequestSearch();
      }
    } else if ((flags & GCS_COMPSTR) != 0) {
      imeComposition_ = readComposition(GCS_COMPSTR);
    }

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos.x = static_cast<LONG>((52.0f + MeasureCaretOffset(query_.substr(0, caret_), static_cast<float>(OverlayWidth()))) *
                                                 GetWindowScale(hwnd_));
    candidate.ptCurrentPos.y = static_cast<LONG>(48.0f * GetWindowScale(hwnd_));
    ImmSetCandidateWindow(context, &candidate);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void OnExtensionActivationReady() {
    ExtensionActivationResult result;
    {
      std::lock_guard lock(extensionActivationMutex_);
      if (!latestExtensionActivation_) {
        extensionActivationPending_ = false;
        return;
      }
      result = std::move(*latestExtensionActivation_);
      latestExtensionActivation_.reset();
    }
    extensionActivationPending_ = false;
    const auto& response = result.response;
    if (!response || !response->handled) {
      ShowTrayNotification(L"FeatherCast Extension",
                           L"The extension did not complete the requested action.");
      return;
    }

    if (response->closeOverlay) HideOverlay(OverlayCloseReason::Action);
    switch (response->action) {
      case feathercast::extensions::HostActionType::OpenUrl:
      case feathercast::extensions::HostActionType::OpenPath:
        if (!response->value.empty()) {
          const HINSTANCE opened =
              ShellExecuteW(nullptr, L"open", response->value.c_str(), nullptr,
                            nullptr, SW_SHOWNORMAL);
          if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            ShowTrayNotification(L"FeatherCast Extension",
                                 L"Windows could not open the extension result.");
          }
        }
        return;
      case feathercast::extensions::HostActionType::CopyText:
        ShowTrayNotification(
            L"FeatherCast",
            CopyTextToClipboard(response->value) ? L"Copied to clipboard."
                                                 : L"Could not copy to clipboard.");
        return;
      case feathercast::extensions::HostActionType::SetQuery:
        SetQueryText(response->value);
        return;
      case feathercast::extensions::HostActionType::None:
        return;
    }
  }

  bool ActivateResultAt(int index, bool asAdmin,
                        unsigned long long expectedGeneration = 0,
                        const std::wstring& expectedKey = L"") {
    if (!ResultsActivationAllowed() || index < 0 ||
        index >= static_cast<int>(flatItems_.size())) {
      return false;
    }
    if (expectedGeneration != 0 &&
        (expectedGeneration != searchPresentation_.Requested() ||
         expectedGeneration != searchPresentation_.Displayed())) {
      return false;
    }
    const DisplayItem& item = flatItems_[static_cast<size_t>(index)];
    if (!expectedKey.empty() && item.Key() != expectedKey) return false;
    Activate(item, asAdmin);
    return true;
  }

  void ActivateCapability(const CapabilityItem& capability) {
    const auto& action = capability.action;
    switch (action.kind) {
      case CapabilityActionKind::SeedQuery:
        SetQueryText(action.query);
        return;
      case CapabilityActionKind::OpenBrowse:
        if (action.browseView == BrowseView::Clipboard &&
            !settings_.clipboardHistoryEnabled) {
          SelectSettingsCategory(SettingsCategory::Privacy);
          OpenSettings();
        } else {
          EnterBrowseView(action.browseView);
        }
        return;
      case CapabilityActionKind::OpenSettings:
        SelectSettingsCategory(action.settingsCategory);
        OpenSettings();
        return;
      case CapabilityActionKind::RunCommand:
        ExecuteCommand(action.command);
        return;
    }
  }

  void Activate(DisplayItem item, bool asAdmin) {
    DebugLaunchLog(L"Activate: isSnippet=" + std::to_wstring(item.isSnippet) +
                   L" isClipboard=" + std::to_wstring(item.isClipboard) +
                   L" isSymbol=" + std::to_wstring(item.isSymbol) +
                   L" isRunCommand=" + std::to_wstring(item.isRunCommand) +
                   L" isCalculator=" + std::to_wstring(item.isCalculator) +
                   L" isUtility=" + std::to_wstring(item.utility.has_value()) +
                   L" isWebSearch=" + std::to_wstring(item.isWebSearch) +
                   L" isExtension=" + std::to_wstring(item.isExtension) +
                   L" isCommand=" + std::to_wstring(item.isCommand) +
                   L" isAction=" + std::to_wstring(item.isAction) +
                   L" isCapability=" + std::to_wstring(item.isCapability) +
                   L" isWindow=" + std::to_wstring(item.isWindow) +
                   L" appName='" + item.app.name + L"' appId='" + item.app.id + L"'" +
                   L" appLaunchTarget='" + item.app.launchTarget + L"'" +
                   L" appType=" + std::to_wstring(static_cast<int>(item.app.launchType)));

    if (item.isCapability) {
      ActivateCapability(item.capability);
      return;
    }

    if (item.isSnippet) {
      PasteTextToLastActiveWindow(item.snippet.text);
      return;
    }

    if (item.isClipboard) {
      PasteTextToLastActiveWindow(item.clipboard.text);
      return;
    }

    if (item.isSymbol) {
      PasteTextToLastActiveWindow(item.symbol.value);
      return;
    }

    if (item.utility) {
      if (CopyTextToClipboard(item.utility->value)) {
        HideOverlay(OverlayCloseReason::Action);
      } else {
        SetOverlayStatus(StatusSeverity::Error,
                         L"Could not copy the utility result.");
      }
      return;
    }

    if (item.isRunCommand) {
      HideOverlay(OverlayCloseReason::Action);
      launchExecutor_.Submit([runCommand = item.runCommand](std::stop_token) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (runCommand.kind == feathercast::run_command::Kind::OpenTarget) {
          ShellExecuteW(nullptr, L"open", runCommand.target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (runCommand.kind == feathercast::run_command::Kind::ShellCommand) {
          const std::wstring args = L"/d /k " + runCommand.input;
          ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        CoUninitialize();
      });
      return;
    }

    if (item.isCalculator || item.isConversion) {
      if (CopyTextToClipboard(item.calculationResult)) {
        HideOverlay(OverlayCloseReason::Action);
        ShowTrayNotification(L"FeatherCast", L"Copied to clipboard.");
      } else {
        ShowTrayNotification(L"FeatherCast", L"Could not copy to clipboard.");
      }
      return;
    }

    if (item.isWebSearch) {
      HideOverlay(OverlayCloseReason::Action);
      launchExecutor_.Submit([url = item.webSearchUrl](std::stop_token) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
      });
      return;
    }

    if (item.isExtension) {
      ActivateExtension(item);
      return;
    }

    if (item.isCommand) {
      ExecuteCommand(item.command);
      return;
    }

    if (item.isAction) {
      ExecuteAction(item);
      return;
    }

    if (item.isWindow) {
      HideOverlay(OverlayCloseReason::Action);
      FocusWindow(item.window.hwnd);
      return;
    }

    HideOverlay(OverlayCloseReason::Action);
    auto appPtr = std::make_shared<AppEntry>(item.app);
    if (!launchExecutor_.Submit([this, appPtr, asAdmin, id = PrimaryAppId(item.app)](std::stop_token stopToken) {
      if (stopToken.stop_requested()) return;
      DebugLaunchLog(L"Launch worker started; type=" +
                     std::to_wstring(static_cast<int>(appPtr->launchType)));
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      const bool ok = this->LaunchApp(*appPtr, asAdmin && appPtr->adminSupported);
      if (!stopToken.stop_requested()) NotifyLaunchCompleted(id, appPtr->name, ok);
      CoUninitialize();
    })) {
      ShowTrayNotification(L"FeatherCast Launch Failed",
                           L"The background launch worker is unavailable.");
    }
  }

  void NotifyLaunchCompleted(const std::wstring& id, const std::wstring& name,
                             bool succeeded) {
    if (stopThreads_) return;
    {
      std::lock_guard lock(completedLaunchMutex_);
      completedLaunches_.push_back({id, name, succeeded});
    }
    if (!PostMessageW(hwnd_, WM_TRACK_RECENT, 0, 0)) {
      std::lock_guard lock(completedLaunchMutex_);
      completedLaunches_.clear();
    }
  }

  void QueueStorageClear(StorageOperationKind kind) {
    if (pendingStorageOperation_) return;
    pendingStorageOperation_ = kind;
    if (kind == StorageOperationKind::ClearFileIndex) {
      // Prevent a currently running index build from repopulating the database
      // after the verified DELETE commits.
      discoveryGeneration_ = discoveryService_.Cancel();
      StopFileIndexService();
    }
    SetSettingsStatus(
        StatusSeverity::Progress,
        kind == StorageOperationKind::ClearClipboard
            ? L"Deleting stored clipboard history..."
            : L"Deleting the stored file index...");

    const bool submitted = persistence_.Clear(kind);
    if (!submitted) {
      pendingStorageOperation_.reset();
      SetSettingsStatus(StatusSeverity::Error,
                        L"The storage worker is unavailable. Data was not deleted.");
    }
    if (settingsHwnd_) InvalidateRect(settingsHwnd_, nullptr, FALSE);
    if (volumeHwnd_) InvalidateRect(volumeHwnd_, nullptr, FALSE);
  }

  void OnStorageOperationReady(StorageOperationResult result) {
    pendingStorageOperation_.reset();
    if (!result.succeeded) {
      SetSettingsStatus(
          StatusSeverity::Error,
          (result.kind == StorageOperationKind::ClearClipboard
               ? L"Clipboard history was not deleted: "
               : L"File index was not deleted: ") +
              (result.error.empty() ? L"Unknown storage error." : result.error));
      if (result.kind == StorageOperationKind::ClearFileIndex) {
        StartAppDiscovery();
      }
      return;
    }

    {
      std::lock_guard lock(dataMutex_);
      if (result.kind == StorageOperationKind::ClearClipboard) {
        clipboardHistory_.clear();
        clipboardSerial_ = 0;
      } else {
        fileIndex_.clear();
      }
    }
    MarkSearchDataChanged();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
    SetSettingsStatus(
        StatusSeverity::Success,
        result.kind == StorageOperationKind::ClearClipboard
            ? L"Stored clipboard history was deleted. New entries may be collected while the feature remains enabled."
            : L"The stored file index was deleted. A future refresh may rebuild it while indexing remains enabled.");
    if (result.kind == StorageOperationKind::ClearFileIndex) {
      StartAppDiscovery(true);
    }
  }

  void ClearClipboardHistoryData() {
    const int choice = MessageBoxW(
        settingsHwnd_ ? settingsHwnd_ : hwnd_,
        L"Delete all currently stored clipboard history?\n\n"
        L"Clipboard History will remain enabled and may collect new entries.",
        L"FeatherCast Privacy",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice == IDYES) QueueStorageClear(StorageOperationKind::ClearClipboard);
  }

  void ClearFileIndexData() {
    const int choice = MessageBoxW(
        settingsHwnd_ ? settingsHwnd_ : hwnd_,
        L"Delete the currently stored file and folder index?\n\n"
        L"File indexing will remain enabled and a future refresh may rebuild it.",
        L"FeatherCast Privacy",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice == IDYES) QueueStorageClear(StorageOperationKind::ClearFileIndex);
  }

  void BeginConfirmation(CommandKind command, std::wstring title,
                         std::wstring message, std::wstring actionLabel) {
    feathercast::ui::OverlayController::ShowConfirmation(
        overlayState_,
        ConfirmationDialog{command, std::move(title), std::move(message),
                            std::move(actionLabel)});
    confirmationClosing_ = false;
    confirmationProgress_.Snap(0.0);
    confirmationProgress_.Retarget(1.0, 0.130, FadeAnimationsAllowed());
    ApplyWindowSize();
    RequestAnimationFrame();
    InvalidateRect(hwnd_, nullptr, FALSE);
    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, 1);
  }

  void FinishCancelConfirmation() {
    if (!confirmation_) return;
    confirmationClosing_ = false;
    feathercast::ui::OverlayController::CloseConfirmation(overlayState_);
    confirmationProgress_.Snap(0.0);
    ApplyWindowSize();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void CancelConfirmation() {
    if (!confirmation_ || confirmationClosing_) return;
    if (FadeAnimationsAllowed()) {
      confirmationClosing_ = true;
      CancelPointerPress(hwnd_);
      confirmationProgress_.Retarget(0.0, 0.090, true);
      RequestAnimationFrame();
      return;
    }
    FinishCancelConfirmation();
  }

  void ActivateConfirmationChoice() {
    if (!confirmation_) return;
    if (confirmationFocus_ == 0) {
      CancelConfirmation();
      return;
    }
    const CommandKind command = confirmation_->command;
    confirmation_.reset();
    confirmationClosing_ = false;
    confirmationProgress_.Snap(0.0);
    confirmationFocus_ = 0;
    confirmationHover_ = -1;
    PerformConfirmedSystemAction(command);
  }

  void PerformConfirmedSystemAction(CommandKind command) {
    HideOverlay(OverlayCloseReason::Action);
    switch (command) {
      case CommandKind::SleepPC:
        SetSuspendState(FALSE, FALSE, FALSE);
        return;
      case CommandKind::ShutDown:
        if (EnableShutdownPrivilege()) {
          ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG,
                        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        }
        return;
      case CommandKind::RestartPC:
        if (EnableShutdownPrivilege()) {
          ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        }
        return;
      case CommandKind::EmptyRecycleBin:
        SHEmptyRecycleBinW(nullptr, nullptr,
                           SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        return;
      default:
        return;
    }
  }

  void ExecuteCommand(CommandKind command) {
    if (const auto* descriptor = feathercast::commands::Find(command);
        descriptor && descriptor->confirmation) {
      const auto& confirmation = *descriptor->confirmation;
      BeginConfirmation(command, confirmation.title, confirmation.message,
                        confirmation.actionLabel);
      return;
    }
    switch (command) {
      case CommandKind::ClipboardHistory:
        if (!settings_.clipboardHistoryEnabled) {
          OpenSettings();
          return;
        }
        EnterBrowseView(BrowseView::Clipboard);
        return;
      case CommandKind::EmojiPicker:
        EnterBrowseView(BrowseView::Emoji);
        return;
      case CommandKind::Games:
        EnterBrowseView(BrowseView::Games);
        return;
      case CommandKind::DiscoverFeatherCast:
        EnterBrowseView(BrowseView::Capabilities);
        return;
      case CommandKind::Settings:
        actionMode_ = false;
        OpenSettings();
        return;
      case CommandKind::Quit:
        DestroyWindow(hwnd_);
        return;
      case CommandKind::Restart:
        RestartApp();
        return;
      case CommandKind::RefreshApps:
        StartAppDiscovery();
        ClearQuery();
        feathercast::ui::OverlayController::ResetResultPosition(overlayState_);
        SyncSelectionAnimationToTarget();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::ClearIconCache:
        ClearIconCache();
        return;
      case CommandKind::ClearRecents:
        settings_.recentApps.clear();
        settings_.usageStats.clear();
        PersistSettings();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::OpenDataFolder:
        launchExecutor_.Submit([path = UserDataPath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          CoUninitialize();
        });
        HideOverlay(OverlayCloseReason::Action);
        return;
      case CommandKind::OpenLocalDataFolder:
        launchExecutor_.Submit([path = LocalDataPath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          CoUninitialize();
        });
        HideOverlay(OverlayCloseReason::Action);
        return;
      case CommandKind::ReloadExtensions:
        launchExecutor_.Submit([this](std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          extensions_.Reload();
          if (!stopToken.stop_requested() && !stopThreads_) {
            PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
          }
        });
        return;
      case CommandKind::CheckForUpdates:
        HideOverlay(OverlayCloseReason::Action);
        StartUpdateCheck(true);
        return;
      case CommandKind::ClearClipboardHistory:
        ClearClipboardHistoryData();
        return;
      case CommandKind::OpenSnippetsFile:
        EnsureSnippetsFile();
        launchExecutor_.Submit([path = SnippetsPath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          CoUninitialize();
        });
        HideOverlay(OverlayCloseReason::Action);
        return;
      case CommandKind::ReloadSnippets:
        ReloadLibraryManagerData();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::OpenThemeFile:
        feathercast::theme::WriteDefaultTheme(ThemePath());
        launchExecutor_.Submit([path = ThemePath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          CoUninitialize();
        });
        HideOverlay(OverlayCloseReason::Action);
        return;
      case CommandKind::ReloadTheme:
        ReloadTheme();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::LockPC:
        HideOverlay(OverlayCloseReason::Action);
        LockWorkStation();
        return;
      case CommandKind::SleepPC:
        return;
      case CommandKind::MuteAudio:
        if (feathercast::audio::ToggleDefaultOutputMute()) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not toggle audio mute.");
        }
        return;
      case CommandKind::ShutDown:
        return;
      case CommandKind::RestartPC:
        return;
      case CommandKind::EmptyRecycleBin:
        return;
      case CommandKind::VolumeControl:
        ShowVolumeControl();
        return;
      case CommandKind::VolumeUp:
        if (feathercast::audio::StepDefaultOutputVolume(true)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not increase the volume.");
        }
        return;
      case CommandKind::VolumeDown:
        if (feathercast::audio::StepDefaultOutputVolume(false)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not decrease the volume.");
        }
        return;
      case CommandKind::MediaPlayPause:
        if (SendVirtualKeyTap(VK_MEDIA_PLAY_PAUSE)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not send the media command.");
        }
        return;
      case CommandKind::MediaNext:
        if (SendVirtualKeyTap(VK_MEDIA_NEXT_TRACK)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not send the media command.");
        }
        return;
      case CommandKind::MediaPrevious:
        if (SendVirtualKeyTap(VK_MEDIA_PREV_TRACK)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not send the media command.");
        }
        return;
      case CommandKind::ShowDesktop:
        if (ToggleDesktop()) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not show the desktop.");
        }
        return;
      case CommandKind::GenerateUuid:
        if (const auto uuid = GenerateUuidText();
            uuid && CopyTextToClipboard(*uuid)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not generate or copy the UUID.");
        }
        return;
    }
  }

  void ExecuteAction(DisplayItem item) {
    if (const auto* windowTarget =
            std::get_if<WindowEntry>(&item.actionTarget)) {
      HWND target = windowTarget->hwnd;
      if (!target || !IsWindow(target)) {
        actionMode_ = false;
        RefreshWindowsAsync();
        RequestSearch();
        SetOverlayStatus(StatusSeverity::Error,
                         L"The selected window is no longer available.");
        return;
      }

      switch (item.action) {
        case ActionKind::ArrangeWindow:
          EnterActionMode(item);
          return;
        case ActionKind::Switch:
          HideOverlay(OverlayCloseReason::Action);
          FocusWindow(target);
          return;
        case ActionKind::Minimize:
          ShowWindowAsync(target, SW_MINIMIZE);
          HideOverlay(OverlayCloseReason::Action);
          return;
        case ActionKind::MaximizeRestore:
          if (IsIconic(target) || IsZoomed(target)) ShowWindowAsync(target, SW_RESTORE);
          else ShowWindowAsync(target, SW_MAXIMIZE);
          HideOverlay(OverlayCloseReason::Action);
          return;
        case ActionKind::MoveWindowLeftHalf:
        case ActionKind::MoveWindowRightHalf:
        case ActionKind::MoveWindowTopHalf:
        case ActionKind::MoveWindowBottomHalf:
        case ActionKind::MoveWindowLeftThird:
        case ActionKind::MoveWindowCenterThird:
        case ActionKind::MoveWindowRightThird:
        case ActionKind::MoveWindowTopLeft:
        case ActionKind::MoveWindowTopRight:
        case ActionKind::MoveWindowBottomLeft:
        case ActionKind::MoveWindowBottomRight:
        case ActionKind::CenterWindow:
        case ActionKind::MoveWindowPreviousDisplay:
        case ActionKind::MoveWindowNextDisplay: {
          feathercast::window_layout::Layout layout =
              feathercast::window_layout::Layout::Center;
          switch (item.action) {
            case ActionKind::MoveWindowLeftHalf:
              layout = feathercast::window_layout::Layout::LeftHalf;
              break;
            case ActionKind::MoveWindowRightHalf:
              layout = feathercast::window_layout::Layout::RightHalf;
              break;
            case ActionKind::MoveWindowTopHalf:
              layout = feathercast::window_layout::Layout::TopHalf;
              break;
            case ActionKind::MoveWindowBottomHalf:
              layout = feathercast::window_layout::Layout::BottomHalf;
              break;
            case ActionKind::MoveWindowLeftThird:
              layout = feathercast::window_layout::Layout::LeftThird;
              break;
            case ActionKind::MoveWindowCenterThird:
              layout = feathercast::window_layout::Layout::CenterThird;
              break;
            case ActionKind::MoveWindowRightThird:
              layout = feathercast::window_layout::Layout::RightThird;
              break;
            case ActionKind::MoveWindowTopLeft:
              layout = feathercast::window_layout::Layout::TopLeft;
              break;
            case ActionKind::MoveWindowTopRight:
              layout = feathercast::window_layout::Layout::TopRight;
              break;
            case ActionKind::MoveWindowBottomLeft:
              layout = feathercast::window_layout::Layout::BottomLeft;
              break;
            case ActionKind::MoveWindowBottomRight:
              layout = feathercast::window_layout::Layout::BottomRight;
              break;
            case ActionKind::MoveWindowPreviousDisplay:
              layout = feathercast::window_layout::Layout::PreviousDisplay;
              break;
            case ActionKind::MoveWindowNextDisplay:
              layout = feathercast::window_layout::Layout::NextDisplay;
              break;
            default:
              break;
          }
          if (ApplyWindowLayout(target, layout)) {
            HideOverlay(OverlayCloseReason::Action);
            FocusWindow(target);
          } else {
            SetOverlayStatus(StatusSeverity::Error,
                             L"Could not arrange the selected window.");
          }
          return;
        }
        case ActionKind::CloseWindow:
          PostMessageW(target, WM_CLOSE, 0, 0);
          HideOverlay(OverlayCloseReason::Action);
          return;
        default:
          return;
      }
    }

    if (const auto* textTarget =
            std::get_if<TextActionPayload>(&item.actionTarget)) {
      if (item.action == ActionKind::CopyText) {
        if (CopyTextToClipboard(textTarget->value)) {
          HideOverlay(OverlayCloseReason::Action);
        } else {
          SetOverlayStatus(StatusSeverity::Error,
                           L"Could not copy the selected text.");
        }
      } else if (item.action == ActionKind::PasteText) {
        PasteTextToLastActiveWindow(textTarget->value);
      }
      return;
    }

    const auto* appTarget = std::get_if<AppEntry>(&item.actionTarget);
    if (!appTarget) return;
    const AppEntry& app = *appTarget;
    const std::wstring id = PrimaryAppId(app);
    switch (item.action) {
      case ActionKind::Preview:
        ExitActionMode();
        previewOpen_ = true;
        ApplyWindowSize();
        ScheduleSelectedPreview();
        return;
      case ActionKind::Open:
      case ActionKind::RunAsAdmin: {
        HideOverlay(OverlayCloseReason::Action);
        auto appPtr = std::make_shared<AppEntry>(app);
        if (!launchExecutor_.Submit([this, appPtr, runAsAdmin = (item.action == ActionKind::RunAsAdmin), id](std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          DebugLaunchLog(L"Action launch worker started; type=" +
                         std::to_wstring(static_cast<int>(appPtr->launchType)));
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          const bool ok = this->LaunchApp(*appPtr, runAsAdmin && appPtr->adminSupported);
          if (!stopToken.stop_requested()) NotifyLaunchCompleted(id, appPtr->name, ok);
          CoUninitialize();
        })) {
          ShowTrayNotification(L"FeatherCast Launch Failed",
                               L"The background launch worker is unavailable.");
        }
        return;
      }
      case ActionKind::OpenLocation:
        RevealAppLocation(app);
        HideOverlay(OverlayCloseReason::Action);
        return;
      case ActionKind::CopyPath:
        CopyTextToClipboard(AppPathForActions(app));
        HideOverlay(OverlayCloseReason::Action);
        return;
      case ActionKind::EditAppAlias:
        HideOverlay(OverlayCloseReason::Action);
        OpenLibraryManager(feathercast::library::ItemKind::AppAlias, id);
        return;
      case ActionKind::Pin:
        if (!id.empty() && !ContainsValue(settings_.pinnedApps, id)) settings_.pinnedApps.insert(settings_.pinnedApps.begin(), id);
        PersistSettings();
        actionMode_ = false;
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case ActionKind::Unpin:
        for (const auto& key : AppKeys(app)) RemoveValue(settings_.pinnedApps, key);
        PersistSettings();
        actionMode_ = false;
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case ActionKind::Hide:
        if (!id.empty() && !ContainsValue(settings_.hiddenApps, id)) settings_.hiddenApps.push_back(id);
        PersistSettings();
        actionMode_ = false;
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case ActionKind::Unhide:
        for (const auto& key : AppKeys(app)) RemoveValue(settings_.hiddenApps, key);
        PersistSettings();
        actionMode_ = false;
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      default:
        return;
    }
  }

  std::wstring AppPathForActions(const AppEntry& app) const {
    if (!app.path.empty()) return app.path;
    if (!app.targetPath.empty()) return app.targetPath;
    return app.launchTarget;
  }

  void RevealAppLocation(const AppEntry& app) const {
    const std::wstring path = AppPathForActions(app);
    if (path.empty()) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  }

  std::optional<std::wstring> ReadClipboardText() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(hwnd_)) return std::nullopt;
    ScopeExit closeClipboard([] { CloseClipboard(); });
    HGLOBAL memory = GetClipboardData(CF_UNICODETEXT);
    if (!memory) return std::nullopt;
    const auto* buffer = static_cast<const wchar_t*>(GlobalLock(memory));
    if (!buffer) return std::nullopt;
    ScopeExit unlockMemory([&] { GlobalUnlock(memory); });
    const size_t available = GlobalSize(memory) / sizeof(wchar_t);
    size_t length = 0;
    while (length < available && buffer[length] != L'\0') ++length;
    length = std::min(length, CLIPBOARD_TEXT_CAP_CHARS);
    return std::wstring(buffer, buffer + length);
  }

  void OnClipboardUpdate() {
    if (!settings_.clipboardHistoryEnabled || settings_.privacyConsentVersion < 1) return;
    auto text = ReadClipboardText();
    if (!text || Trim(*text).empty()) return;
    if (internalClipboardText_ && *text == *internalClipboardText_) {
      internalClipboardText_.reset();
      return;
    }
    {
      std::lock_guard lock(dataMutex_);
      if (!clipboardHistory_.empty() && clipboardHistory_.front().text == *text) return;
    }

    const long long capturedAt = UnixNow();
    const std::wstring preview = SingleLinePreview(*text);
    ClipboardEntry entry;
    entry.id = std::to_wstring(++clipboardSerial_);
    entry.text = *text;
    entry.preview = preview;
    entry.capturedAt = capturedAt;
    {
      std::lock_guard lock(dataMutex_);
      clipboardHistory_.erase(
          std::remove_if(clipboardHistory_.begin(), clipboardHistory_.end(),
                         [&](const ClipboardEntry& existing) { return existing.text == entry.text; }),
          clipboardHistory_.end());
      clipboardHistory_.insert(clipboardHistory_.begin(), std::move(entry));
      if (clipboardHistory_.size() > ClipboardHistoryLimit()) clipboardHistory_.resize(ClipboardHistoryLimit());
    }
    const std::wstring storedText = *text;
    const size_t retention = ClipboardHistoryLimit();
    if (!persistence_.StoreClipboard(storedText, preview, capturedAt,
                                     retention)) {
      ReportPersistenceFailure(L"The persistence worker is unavailable.");
    }
    MarkSearchDataChanged();  // clipboard history feeds the search corpus
    if (visible_) {
      RequestSearch();
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

  bool CopyTextToClipboard(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(hwnd_)) return false;
    ScopeExit closeClipboard([] { CloseClipboard(); });
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    ScopeExit freeMemory([&] { GlobalFree(memory); });
    void* buffer = GlobalLock(memory);
    if (!buffer) return false;
    memcpy(buffer, text.c_str(), bytes);
    GlobalUnlock(memory);
    // Ownership of the global block transfers to the clipboard on success.
    if (!SetClipboardData(CF_UNICODETEXT, memory)) return false;
    freeMemory.Release();
    internalClipboardText_ = text.substr(0, CLIPBOARD_TEXT_CAP_CHARS);
    return true;
  }

  void SendPasteShortcut() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = L'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = L'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT));
  }

  void PasteTextToLastActiveWindow(const std::wstring& text) {
    HWND target = overlayRestoreCandidate_ ? overlayRestoreCandidate_->hwnd
                                           : nullptr;
    const UINT existingFormats = CountClipboardFormats();
    ComPtr<IDataObject> previousClipboard;
    const HRESULT captureResult = OleGetClipboard(&previousClipboard);
    if (existingFormats > 0 && FAILED(captureResult)) {
      ShowTrayNotification(L"FeatherCast Paste",
                           L"The current clipboard could not be preserved, so FeatherCast did not replace it.");
      return;
    }

    HideOverlay(OverlayCloseReason::Action);
    if (!CopyTextToClipboard(text)) {
      ShowTrayNotification(L"FeatherCast Paste", L"FeatherCast could not place the text on the clipboard.");
      return;
    }
    const DWORD temporarySequence = GetClipboardSequenceNumber();
    overlayRestoreCandidate_.reset();

    if (!launchExecutor_.Submit(
            [this, target, previousClipboard, existingFormats, temporarySequence](std::stop_token stopToken) {
              CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
              ScopeExit uninitialize([] { CoUninitialize(); });
              if (stopToken.stop_requested()) return;
              if (target && IsWindow(target)) {
                FocusWindow(target);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (stopToken.stop_requested()) return;
                SendPasteShortcut();
              }

              std::this_thread::sleep_for(std::chrono::milliseconds(400));
              if (stopToken.stop_requested() ||
                  GetClipboardSequenceNumber() != temporarySequence) {
                return;
              }

              if (existingFormats > 0 && previousClipboard) {
                if (SUCCEEDED(OleSetClipboard(previousClipboard.Get()))) OleFlushClipboard();
              } else if (OpenClipboard(hwnd_)) {
                EmptyClipboard();
                CloseClipboard();
              }
            })) {
      ShowTrayNotification(L"FeatherCast Paste", L"FeatherCast could not send the paste operation.");
    }
  }

  void ClearIconCache() {
    StopIconThreads();
    iconResolver_.ClearPending();
    ClearIconBitmaps();
    std::error_code ec;
    std::filesystem::remove_all(IconCacheDir(), ec);
    std::filesystem::create_directories(IconCacheDir(), ec);
    if (!stopThreads_ && visible_) StartIconWorkers();
    // App discovery is useful for the launcher immediately, but the file
    // index is demand-loaded when the Files scope is first selected.
    StartAppDiscovery();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void StopIconThreads() {
    iconResolver_.Stop();
  }

  void RestartApp() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring command = L"/c timeout /t 1 /nobreak >nul & start \"\" \"" + std::wstring(exePath) + L"\" --show";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", command.c_str(), nullptr, SW_HIDE);
    DestroyWindow(hwnd_);
  }

  bool LaunchApp(const AppEntry& app, bool asAdmin) {
    DebugLaunchLog(L"LaunchApp type=" +
                   std::to_wstring(static_cast<int>(app.launchType)) + L" admin=" +
                   (asAdmin ? L"1" : L"0"));

    if (app.launchType == LaunchType::Shell) {
      HINSTANCE result = ShellExecuteW(nullptr, L"open", app.launchTarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      DebugLaunchLog(L"  Shell result=" + std::to_wstring(reinterpret_cast<intptr_t>(result)));
      return reinterpret_cast<intptr_t>(result) > 32;
    }

    if (app.launchType == LaunchType::AppsFolder) {
      if (asAdmin) {
        // Prefer the execution alias (e.g. wt.exe) when we have one — it is a
        // real executable that the runas verb understands natively.
        if (!app.targetPath.empty()) {
          SHELLEXECUTEINFOW sei{};
          sei.cbSize = sizeof(sei);
          sei.fMask = SEE_MASK_NOASYNC;
          sei.lpVerb = L"runas";
          sei.lpFile = app.targetPath.c_str();
          sei.nShow = SW_SHOWNORMAL;
          if (ShellExecuteExW(&sei)) return true;
        }
        // Fallback for packaged/Store apps without a known alias: open the
        // shell:AppsFolder item with the runas verb so the user gets a UAC
        // prompt regardless of the app's packaging model.
        const std::wstring adminTarget = L"shell:AppsFolder\\" + app.launchTarget;
        SHELLEXECUTEINFOW seiShell{};
        seiShell.cbSize = sizeof(seiShell);
        seiShell.fMask = SEE_MASK_NOASYNC;
        seiShell.lpVerb = L"runas";
        seiShell.lpFile = adminTarget.c_str();
        seiShell.nShow = SW_SHOWNORMAL;
        DebugLaunchLog(L"  AppsFolder admin fallback requested");
        if (ShellExecuteExW(&seiShell)) return true;
        // Never silently downgrade an explicit elevated launch to a normal one.
        return false;
      }

      // Prefer the activation manager: it launches packaged apps (Terminal,
      // Store apps) by AUMID and brings the window to the foreground. Passing
      // the AUMID to explorer.exe occasionally just opens the Applications
      // folder, which is the behavior we want to avoid.
      ComPtr<IApplicationActivationManager> activator;
      HRESULT hrCreate = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                          CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&activator));
      DebugLaunchLog(L"  AppsFolder CoCreate hr=0x" + ToHex(static_cast<unsigned>(hrCreate)));
      if (SUCCEEDED(hrCreate)) {
        DWORD pid = 0;
        HRESULT hrAct = activator->ActivateApplication(app.launchTarget.c_str(), nullptr, AO_NONE, &pid);
        DebugLaunchLog(L"  ActivateApplication hr=0x" + ToHex(static_cast<unsigned>(hrAct)) +
                       L" pid=" + std::to_wstring(pid));
        if (SUCCEEDED(hrAct)) {
          return true;
        }
      }

      // Fallback for classic Start-menu AUMIDs the activation manager can't
      // handle: open the shell item directly rather than via explorer.exe.
      const std::wstring target = L"shell:AppsFolder\\" + app.launchTarget;
      HINSTANCE result = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      DebugLaunchLog(L"  fallback ShellExecute result=" +
                     std::to_wstring(reinterpret_cast<intptr_t>(result)));
      return reinterpret_cast<intptr_t>(result) > 32;
    }

    std::wstring file = app.launchTarget;
    std::wstring args = app.args;
    std::wstring cwd = app.cwd;

    if (asAdmin && app.launchType == LaunchType::Shortcut) {
      ShortcutInfo info;
      if (LoadShortcut(app.launchTarget, info) && !info.target.empty()) {
        file = info.target;
        args = info.args;
        cwd = info.cwd;
      }
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = asAdmin ? L"runas" : L"open";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = cwd.empty() ? nullptr : cwd.c_str();
    sei.nShow = SW_SHOWNORMAL;
    BOOL ok = ShellExecuteExW(&sei);
    DebugLaunchLog(L"  Shortcut/Exe ok=" + std::to_wstring(ok) +
                   L" err=" + std::to_wstring(GetLastError()) +
                   L" hInst=" + std::to_wstring(reinterpret_cast<intptr_t>(sei.hInstApp)));
    return ok == TRUE;
  }

  void TrackRecent(const std::wstring& id) {
    if (id.empty()) return;
    std::vector<std::wstring> next{id};
    for (const auto& existing : settings_.recentApps) {
      if (existing != id) next.push_back(existing);
      if (next.size() >= RECENT_LIMIT) break;
    }
    settings_.recentApps = std::move(next);
    auto& usage = settings_.usageStats[id];
    usage.launches = std::min(usage.launches + 1, 1000000);
    usage.lastUsed = UnixNow();
    PersistSettings();
  }

  void OnTray(LPARAM lParam) {
    const UINT event = LOWORD(lParam);
    if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
      ShowOverlay(View::Search);
    } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
      POINT pt{};
      GetCursorPos(&pt);
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, 1, L"Open FeatherCast");
      AppendMenuW(menu, MF_STRING, 2, L"Settings");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, 3, L"Quit");
      SetForegroundWindow(hwnd_);
      const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
      DestroyMenu(menu);
      if (cmd == 1) ShowOverlay(View::Search);
      else if (cmd == 2) OpenSettings();
      else if (cmd == 3) DestroyWindow(hwnd_);
    }
  }

  feathercast::runtime::UiEventQueue<feathercast::persistence::Event>
      persistenceEvents_;
  feathercast::persistence::PersistenceService persistence_;
  feathercast::runtime::UiEventQueue<feathercast::search::CoordinatorEvent>
      searchEvents_;
  feathercast::search::SearchCoordinator searchCoordinator_;
  feathercast::search::SnapshotCoordinator snapshotCoordinator_;
  feathercast::runtime::UiEventQueue<DiscoveryResult> discoveryEvents_;
  feathercast::discovery_runtime::DiscoveryService discoveryService_;
  feathercast::runtime::UiEventQueue<feathercast::files::IndexStatus>
      fileIndexEvents_;
  feathercast::files::FileIndexService fileIndexService_;
  feathercast::runtime::UiEventQueue<ResultsCollection> fileSearchEvents_;
  feathercast::files::FileSearchService fileSearchService_;
  feathercast::runtime::UiEventQueue<feathercast::preview::Result>
      previewEvents_;
  feathercast::preview::PreviewService previewService_;
  feathercast::runtime::UiEventQueue<feathercast::runtime::DecodedIcon>
      iconEvents_;
  feathercast::runtime::IconResolver iconResolver_;
  feathercast::runtime::UiEventQueue<NetworkEvent> networkEvents_;
  feathercast::runtime::CurrencyService<CurrencyRates> currencyService_;
  feathercast::runtime::UpdateService<UpdateTaskResult> updateService_;
  HINSTANCE instance_ = nullptr;
  std::wstring cmdLine_;
  HWND hwnd_ = nullptr;
  HWND settingsHwnd_ = nullptr;
  HWND volumeHwnd_ = nullptr;
  NOTIFYICONDATAW tray_{};
  HHOOK hook_ = nullptr;
  bool hotKeyRegistered_ = false;
  bool clipboardListenerRegistered_ = false;
  bool hadLegacyOperationalData_ = false;
  UINT taskbarCreatedMessage_ = 0;
  ULONGLONG lastShortcutToggleTick_ = 0;
  std::uint64_t nextShortcutToggleRequestId_ = 0;
  std::uint64_t pendingShortcutToggleRequestId_ = 0;
  std::deque<ShortcutToggleRequest> shortcutToggleRequests_;
  feathercast::interaction::OverlayFocusSession overlayFocusSession_;
  bool overlayReactivationQueued_ = false;
  Settings settings_;
  bool settingsPersistenceBlocked_ = false;
  bool settingsRecoveredFromInvalidFile_ = false;
  std::wstring startupSettingsNotice_;
  std::optional<StatusMessage> settingsStatus_;
  ShortcutSpec shortcut_;
  std::optional<RestoreCandidate> overlayRestoreCandidate_;
  HWND volumeRestoreWindow_ = nullptr;
  bool visible_ = false;
  bool volumeVisible_ = false;
  bool volumeDragging_ = false;
  int volumePercent_ = 0;
  std::wstring volumeStatus_;
  HMONITOR volumeMonitor_ = nullptr;
  bool highContrast_ = false;
  bool systemAnimationsEnabled_ = true;
  bool suppressHide_ = false;
  LARGE_INTEGER qpcFrequency_{};
  LONGLONG lastAnimationFrameQpc_ = 0;
  bool animating_ = false;
  bool animationFrameQueued_ = false;
  bool animationLoopRunning_ = false;
  feathercast::motion::AnimationTimeline overlayRevealTimeline_;
  feathercast::motion::ScalarAnimation overlayOpacity_;
  feathercast::motion::ScalarAnimation settingsOpacity_;
  feathercast::motion::ScalarAnimation volumeOpacity_;
  feathercast::motion::ScalarAnimation confirmationProgress_;
  feathercast::motion::ScalarAnimation overlayVisualScroll_;
  feathercast::motion::ScalarAnimation settingsVisualScroll_;
  feathercast::motion::ScalarAnimation settingsPageProgress_;
  feathercast::motion::ScalarAnimation settingsCategoryTop_;
  feathercast::motion::ScalarAnimation volumeVisualPercent_;
  feathercast::motion::AnimatedBounds overlayBounds_;
  feathercast::motion::AnimatedBounds settingsBounds_;
  feathercast::motion::AnimatedBounds volumeBounds_;
  feathercast::motion::PendingNavigation pendingNavigation_;
  std::map<HitType, feathercast::motion::ScalarAnimation> switchAnimations_;
  struct ResultElementMotion {
    feathercast::motion::ScalarAnimation offsetY;
    feathercast::motion::ScalarAnimation opacity;
  };
  std::map<std::wstring, ResultElementMotion> resultRowMotion_;
  std::map<std::wstring, ResultElementMotion> resultHeaderMotion_;
  std::wstring displayedQuery_;
  bool overlayClosing_ = false;
  bool settingsClosing_ = false;
  bool volumeClosing_ = false;
  bool confirmationClosing_ = false;
  std::optional<PendingOverlayClose> pendingOverlayClose_;
  HWND pendingVolumeRestore_ = nullptr;
  bool surfaceResizeFailed_ = false;
  bool forceWarp_ = false;
  unsigned deviceLossCount_ = 0;
  bool renderRecoveryQueued_ = false;
  float visualSelectedY_ = -1.0f;
  bool animatingSelection_ = false;
  double selectionSettleSeconds_ = 0.090;
  feathercast::ui::OverlayState overlayState_;
  View& view_ = overlayState_.view;
  HMONITOR overlayMonitor_ = nullptr;
  std::wstring& query_ = overlayState_.query;
  std::wstring& imeComposition_ = overlayState_.imeComposition;
  size_t& caret_ = overlayState_.caret;
  std::optional<size_t>& selectionAnchor_ = overlayState_.selectionAnchor;
  int& selected_ = overlayState_.selected;
  int& scroll_ = overlayState_.scroll;
  bool& actionMode_ = overlayState_.actionMode;
  DisplayItem& actionTarget_ = overlayState_.actionTarget;
  BrowseView& browseView_ = overlayState_.browseView;
  std::optional<ConfirmationDialog>& confirmation_ = overlayState_.confirmation;
  std::optional<StatusMessage>& overlayStatus_ = overlayState_.status;
  int& confirmationFocus_ = overlayState_.confirmationFocus;
  int& confirmationHover_ = overlayState_.confirmationHover;
  bool suppressNextChar_ = false;
  std::vector<NavigationState>& navigationStack_ = overlayState_.navigationStack;
  std::optional<NavigationState>& pendingNavigationRestore_ =
      overlayState_.pendingNavigationRestore;
  feathercast::ui::SettingsState settingsState_;
  bool& recording_ = settingsState_.recordingShortcut;
  bool gearHovered_ = false;
  bool mouseTracking_ = false;
  bool ignoreMouseUntilMove_ = false;
  POINT mouseAnchor_ = {0, 0};
  int& settingsHover_ = settingsState_.hover;
  int& settingsFocusIndex_ = settingsState_.focusIndex;
  float& settingsScroll_ = settingsState_.scroll;
  SettingsCategory& settingsCategory_ = settingsState_.category;
  std::wstring& pendingShortcut_ = settingsState_.pendingShortcut;
  ShortcutRecorder shortcutRecorder_;
  ShortcutRuntime shortcutRuntime_;
  PressedModifiers hookModifiers_;
  std::atomic<bool> stopThreads_ = false;
  std::atomic<bool> shutdownStarted_ = false;
  std::atomic<bool> caching_ = false;
  std::atomic<bool> windowRefreshPending_ = false;
  feathercast::runtime::LaunchService launchExecutor_;
  std::optional<StorageOperationKind> pendingStorageOperation_;
  std::mutex completedLaunchMutex_;
  std::deque<LaunchCompletion> completedLaunches_;
  std::atomic<bool> appsReady_ = false;
  std::uint64_t discoveryGeneration_ = 0;
  std::uint64_t fileIndexGeneration_ = 0;
  bool fileIndexServiceStarted_ = false;
  bool fileIndexConfigured_ = false;
  bool fileIndexLoaded_ = false;
  std::uint64_t previewGeneration_ = 0;
  uint64_t snapshotScheduledRevision_ = 0;
  feathercast::interaction::SearchPresentationState searchPresentation_;
  // Cached query-independent search corpus, rebuilt when the published data
  // revision changes. Revisions cannot lose a concurrent invalidation.
  std::shared_ptr<const SearchSnapshot> snapshot_;
  std::atomic<uint64_t> dataRevision_ = 1;
  uint64_t snapshotRevision_ = 0;
  std::mutex dataMutex_;
  feathercast::theme::Theme theme_;
  std::vector<AppEntry> apps_;
  std::vector<WindowEntry> windows_;
  std::vector<AppEntry> fileIndex_;
  std::optional<feathercast::files::IndexStatus> fileIndexStatus_;
  bool previewOpen_ = false;
  float previewScroll_ = 0.0f;
  std::optional<feathercast::preview::Result> previewResult_;
  ComPtr<ID2D1Bitmap> previewBitmap_;
  std::vector<AppEntry> systemFolders_;
  std::vector<feathercast::snippets::Snippet> snippets_;
  feathercast::snippets_io::FileFingerprint snippetsFingerprint_;
  bool snippetsWritable_ = true;
  std::wstring snippetsLoadMessage_;
  std::vector<ClipboardEntry> clipboardHistory_;
  unsigned long long clipboardSerial_ = 0;
  std::optional<std::wstring> internalClipboardText_;
  CurrencyRates currencyRates_;
  std::wstring localeCurrency_ = DetectLocaleCurrency();
  feathercast::extensions::ExtensionManager extensions_;
  std::mutex extensionActivationMutex_;
  std::optional<ExtensionActivationResult> latestExtensionActivation_;
  bool extensionActivationPending_ = false;
  bool extensionReloadPending_ = false;
  std::vector<Section> sections_;
  std::vector<DisplayItem> flatItems_;
  feathercast::ui::HitRegions hits_;
  std::optional<PointerPress> pointerPress_;
  // LRU-bounded in-memory bitmap cache. iconLru_ holds keys most-recent-first;
  // each map entry stores its position so it can be promoted/evicted in O(1).
  struct IconCacheEntry {
    ComPtr<ID2D1Bitmap> bitmap;
    std::list<std::wstring>::iterator lruIt;
  };
  std::list<std::wstring> iconLru_;
  std::unordered_map<std::wstring, IconCacheEntry> iconBitmaps_;
  std::unordered_map<std::wstring, feathercast::runtime::DecodedIcon>
      pendingDecodedIcons_;
  static constexpr size_t kIconCacheCap = 256;
  static constexpr size_t kPendingDecodedIconCap = 256;
  static constexpr size_t kPendingDecodedIconBudget = 16 * 1024 * 1024;
  std::deque<std::wstring> pendingDecodedIconOrder_;
  size_t pendingDecodedIconBytes_ = 0;

  ComPtr<IDWriteFactory> dwriteFactory_;
  ComPtr<IWICImagingFactory> wicFactory_;

  // Shared Direct3D 11 / Direct2D / DirectComposition device stack (see EnsureGlassDevice).
  ComPtr<ID3D11Device> d3dDevice_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<IDXGIFactory2> dxgiFactory_;
  ComPtr<IDCompositionDevice> dcompDevice_;

  GlassSurface overlaySurface_;
  GlassSurface settingsSurface_;
  GlassSurface volumeSurface_;
  bool overlayBlurApplied_ = false;
  bool settingsBlurApplied_ = false;
  bool volumeBlurApplied_ = false;
  std::wstring caretMeasureText_ = L"\x01";  // sentinel that never equals a real measured prefix
  float caretMeasureWidth_ = -1.0f;
  float caretOffset_ = 0.0f;
  bool lastCaretPhase_ = false;
  ID2D1RenderTarget* activeRT_ = nullptr;
  // Per-render-target cache of solid-color brushes, keyed by packed RGBA.
  std::unordered_map<ID2D1RenderTarget*, std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>>> brushCache_;
  HRESULT lastBrushFailure_ = S_OK;
  ComPtr<ID2D1DeviceContext> activeDC_;  // QI of activeRT_ for color-emoji DrawText, when available
  ComPtr<ID2D1StrokeStyle> resultIconStroke_;
  ComPtr<IDWriteTextFormat> inputFormat_;
  ComPtr<IDWriteTextFormat> rowFormat_;
  ComPtr<IDWriteTextFormat> calculationResultFormat_;
  ComPtr<IDWriteTextFormat> subFormat_;
  ComPtr<IDWriteTextFormat> sectionFormat_;
  ComPtr<IDWriteTextFormat> footerFormat_;
  ComPtr<IDWriteTextFormat> footerRightFormat_;
  ComPtr<IDWriteTextFormat> titleFormat_;
  ComPtr<IDWriteTextFormat> labelFormat_;
  ComPtr<IDWriteTextFormat> bodyFormat_;
  ComPtr<IDWriteTextFormat> buttonFormat_;
  ComPtr<IDWriteTextFormat> centerFormat_;
  ComPtr<IDWriteTextFormat> emojiFormat_;
  ComPtr<IDWriteTextFormat> volumeValueFormat_;
};

}  // namespace

int RunFeatherCastSelfTest() {
  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(coHr)) return 10;

  ComPtr<IDWriteFactory> writeFactory;
  const HRESULT writeHr = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()));

  ComPtr<IWICImagingFactory> imagingFactory;
  const HRESULT imagingHr = CoCreateInstance(
      CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&imagingFactory));

  ComPtr<ID2D1Factory1> d2dFactory;
  const HRESULT d2dHr = D2D1CreateFactory(
      D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
  if (FAILED(writeHr) || FAILED(imagingHr) || FAILED(d2dHr)) {
    d2dFactory.Reset();
    imagingFactory.Reset();
    writeFactory.Reset();
    CoUninitialize();
    return 11;
  }

  ComPtr<ID3D11Device> d3dDevice;
  ComPtr<ID3D11DeviceContext> d3dContext;
  ComPtr<ID2D1Device> d2dDevice;
  ComPtr<ID2D1DeviceContext> renderContext;
  auto recreateDevice = [&]() -> HRESULT {
    renderContext.Reset();
    d2dDevice.Reset();
    d3dContext.Reset();
    d3dDevice.Reset();
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice, &featureLevel, &d3dContext);
    if (FAILED(result)) return result;
    ComPtr<IDXGIDevice> dxgiDevice;
    result = d3dDevice.As(&dxgiDevice);
    if (FAILED(result)) return result;
    result = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (FAILED(result)) return result;
    return d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &renderContext);
  };

  // A windowless render-target lifecycle stress test. Recreating the complete
  // WARP stack every 50 iterations simulates recovery from device loss. Each
  // pass also mirrors the search overlay's transparent frame, clipped results,
  // and per-row transition layer so typing regressions fail without showing or
  // focusing an HWND.
  HRESULT lifecycleResult = S_OK;
  int lifecycleFailureExit = 12;
  for (unsigned cycle = 0; cycle < 500; ++cycle) {
    if (cycle % 50 == 0) {
      lifecycleResult = recreateDevice();
      if (FAILED(lifecycleResult)) break;
    }
    const float dpiValues[] = {96.0f, 120.0f, 144.0f, 192.0f};
    const float dpi = dpiValues[cycle % ARRAYSIZE(dpiValues)];
    const UINT width = 480 + (cycle % 9) * 37;
    const UINT height = 240 + (cycle % 7) * 29;
    ComPtr<ID2D1Bitmap1> target;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi);
    lifecycleResult = renderContext->CreateBitmap(
        D2D1::SizeU(width, height), nullptr, 0, &properties, &target);
    if (FAILED(lifecycleResult)) break;
    renderContext->SetTarget(target.Get());
    renderContext->SetDpi(dpi, dpi);

    ComPtr<IDWriteTextFormat> format;
    lifecycleResult = writeFactory->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f + static_cast<float>(cycle % 3), L"en-US", &format);
    if (FAILED(lifecycleResult)) break;
    ComPtr<ID2D1SolidColorBrush> brush;
    const bool highContrastPass = (cycle % 2) != 0;
    lifecycleResult = renderContext->CreateSolidColorBrush(
        highContrastPass ? D2D1::ColorF(D2D1::ColorF::White)
                         : D2D1::ColorF(0.24f, 0.52f, 0.96f, 1.0f),
        &brush);
    if (FAILED(lifecycleResult)) break;
    ComPtr<ID2D1SolidColorBrush> panelBrush;
    lifecycleResult = renderContext->CreateSolidColorBrush(
        highContrastPass ? D2D1::ColorF(D2D1::ColorF::Black)
                         : D2D1::ColorF(0.04f, 0.05f, 0.07f, 0.72f),
        &panelBrush);
    if (FAILED(lifecycleResult)) break;
    renderContext->BeginDraw();
    renderContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    renderContext->FillRectangle(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                    static_cast<float>(height)),
        panelBrush.Get());
    renderContext->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(16.0f, 20.0f, 32.0f, 36.0f),
                          4.0f, 4.0f),
        brush.Get());
    static constexpr wchar_t kQuery[] = L"FeatherCast search";
    renderContext->DrawTextW(
        kQuery, ARRAYSIZE(kQuery) - 1, format.Get(),
        D2D1::RectF(16.0f, 16.0f, static_cast<float>(width - 16), 52.0f),
        brush.Get());
    const D2D1_RECT_F resultsClip = D2D1::RectF(
        0.0f, 60.0f, static_cast<float>(width),
        static_cast<float>(height - 40));
    renderContext->PushAxisAlignedClip(resultsClip,
                                       D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_MATRIX_3X2_F baseTransform{};
    renderContext->GetTransform(&baseTransform);
    const float rowOffset = static_cast<float>(cycle % 5);
    renderContext->SetTransform(
        D2D1::Matrix3x2F::Translation(0.0f, rowOffset) * baseTransform);
    renderContext->PushLayer(
        D2D1::LayerParameters(
            D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
            0.65f + static_cast<float>(cycle % 4) * 0.1f),
        nullptr);
    renderContext->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(8.0f, 76.0f, static_cast<float>(width - 8), 126.0f),
            8.0f, 8.0f),
        brush.Get());
    static constexpr wchar_t kResult[] = L"Visible search result";
    renderContext->DrawTextW(
        kResult, ARRAYSIZE(kResult) - 1, format.Get(),
        D2D1::RectF(20.0f, 88.0f, static_cast<float>(width - 20), 118.0f),
        panelBrush.Get());
    renderContext->PopLayer();
    renderContext->SetTransform(baseTransform);
    static constexpr wchar_t kStatus[] = L"Searching...";
    renderContext->DrawTextW(
        kStatus, ARRAYSIZE(kStatus) - 1, format.Get(),
        D2D1::RectF(static_cast<float>(width - 140), 64.0f,
                    static_cast<float>(width - 16), 92.0f),
        brush.Get());
    renderContext->PopAxisAlignedClip();
    lifecycleResult = renderContext->EndDraw();
    renderContext->SetTarget(nullptr);
    if (FAILED(lifecycleResult)) break;

    if (cycle % 25 == 0) {
      ComPtr<ID2D1Bitmap1> readback;
      const auto readbackProperties = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED),
          dpi, dpi);
      lifecycleResult = renderContext->CreateBitmap(
          D2D1::SizeU(width, height), nullptr, 0, &readbackProperties,
          &readback);
      if (FAILED(lifecycleResult)) {
        lifecycleFailureExit = 13;
        break;
      }
      lifecycleResult = readback->CopyFromBitmap(nullptr, target.Get(), nullptr);
      if (FAILED(lifecycleResult)) {
        lifecycleFailureExit = 14;
        break;
      }
      D2D1_MAPPED_RECT mapped{};
      lifecycleResult = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped);
      if (FAILED(lifecycleResult)) {
        lifecycleFailureExit = 15;
        break;
      }
      const BYTE* panelPixel = mapped.bits + 2 * mapped.pitch + 2 * 4;
      const float pixelScale = dpi / 96.0f;
      const UINT searchX = std::min<UINT>(
          width - 1, static_cast<UINT>(std::lround(24.0f * pixelScale)));
      const UINT searchY = std::min<UINT>(
          height - 1, static_cast<UINT>(std::lround(28.0f * pixelScale)));
      const BYTE* searchPixel =
          mapped.bits + static_cast<size_t>(searchY) * mapped.pitch +
          static_cast<size_t>(searchX) * 4;
      const bool hasVisiblePixel = panelPixel[3] != 0;
      const bool searchContentDiffersFromPanel =
          panelPixel[0] != searchPixel[0] ||
          panelPixel[1] != searchPixel[1] ||
          panelPixel[2] != searchPixel[2] ||
          panelPixel[3] != searchPixel[3];
      const HRESULT unmapResult = readback->Unmap();
      if (FAILED(unmapResult)) {
        lifecycleResult = unmapResult;
        lifecycleFailureExit = 16;
        break;
      }
      if (!hasVisiblePixel || !searchContentDiffersFromPanel) {
        lifecycleResult = E_FAIL;
        lifecycleFailureExit = 17;
        break;
      }
    }
  }

  renderContext.Reset();
  d2dDevice.Reset();
  d3dContext.Reset();
  d3dDevice.Reset();
  d2dFactory.Reset();
  imagingFactory.Reset();
  writeFactory.Reset();
  CoUninitialize();
  return SUCCEEDED(lifecycleResult) ? 0 : lifecycleFailureExit;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
  const std::wstring cmdLineStr = cmdLine ? cmdLine : L"";
  if (cmdLineStr.find(L"--self-test") != std::wstring::npos) {
    return RunFeatherCastSelfTest();
  }

  UniqueHandle mutex(CreateMutexW(nullptr, TRUE, kMutexName));
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    for (int attempt = 0; attempt < 40; ++attempt) {
      HWND existing = FindWindowW(kWindowClass, L"FeatherCast");
      if (existing) {
        DWORD_PTR acknowledged = 0;
        if (SendMessageTimeoutW(existing, WM_SHOW_SEARCH, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged)) {
          return 0;
        }
      }
      Sleep(50);
    }
    MessageBoxW(nullptr, L"FeatherCast is already running but could not be activated.",
                L"FeatherCast", MB_OK | MB_ICONWARNING);
    return 1;
  }

  FeatherCastApp app(instance, cmdLineStr);
  return app.Run();
}
