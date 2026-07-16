#include "calculator.hpp"
#include "accessibility.hpp"
#include "background_executor.hpp"
#include "converter.hpp"
#include "core.hpp"
#include "discovery.hpp"
#include "emoji.hpp"
#include "extension_manager.hpp"
#include "json.hpp"
#include "run_command.hpp"
#include "settings.hpp"
#include "shortcut.hpp"
#include "snippets.hpp"
#include "storage.hpp"
#include "symbols.hpp"
#include "theme.hpp"
#include "text_edit.hpp"
#include "updater.hpp"
#include "version.hpp"

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
#include <endpointvolume.h>
#include <knownfolders.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <propidl.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
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
using feathercast::shortcut::IsModifier;
using feathercast::shortcut::ModifierPressed;
using feathercast::shortcut::ParseShortcut;
using feathercast::shortcut::PressedModifiers;
using feathercast::shortcut::ShortcutRecorder;
using feathercast::shortcut::ShortcutRuntime;
using feathercast::shortcut::ShortcutSpec;
using feathercast::shortcut::ShouldHandleInLowLevelHook;
using feathercast::shortcut::ToHotKeySpec;

namespace {

constexpr int IDI_APP_ICON = 101;
constexpr wchar_t kWindowClass[] = L"FeatherCastNativeWindow";
constexpr wchar_t kSettingsWindowClass[] = L"FeatherCastSettingsWindow";
constexpr wchar_t kMutexName[] = L"FeatherCastNativeSingleInstance";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SHOW_SEARCH = WM_APP + 2;
constexpr UINT WM_ICON_READY = WM_APP + 3;
constexpr UINT WM_REBUILD_RESULTS = WM_APP + 4;
constexpr UINT WM_SEARCH_READY = WM_APP + 5;
constexpr UINT WM_SHORTCUT_TOGGLE = WM_APP + 6;
constexpr UINT WM_UPDATE_READY = WM_APP + 7;
constexpr UINT WM_TRACK_RECENT = WM_APP + 8;
constexpr UINT WM_ANIMATION_FRAME = WM_APP + 9;
constexpr UINT WM_EXTENSION_ACTIVATED = WM_APP + 10;
constexpr UINT WM_SNAPSHOT_READY = WM_APP + 11;
constexpr UINT WM_PERSISTENCE_ERROR = WM_APP + 12;
constexpr int HOTKEY_OPEN_SEARCH = 0x4C43;
constexpr int HOTKEY_VALIDATE_SHORTCUT = 0x4C44;
constexpr UINT TIMER_MEM_TRIM = 3;
constexpr UINT TIMER_SELECTION_ANIM = 4;

constexpr int WIN_WIDTH = 720;
constexpr int WIN_HEIGHT = 470;
constexpr int COMPACT_BASE_HEIGHT = 60;
constexpr int RECENT_LIMIT = 8;
constexpr int MAX_RESULTS = 200;
constexpr int MIN_OVERLAY_WIDTH = 560;
constexpr int MAX_OVERLAY_WIDTH = 980;
constexpr int SETTINGS_WIDTH = 580;
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

enum class View {
  Search,
  Settings,
};

enum class LaunchType {
  Shortcut,
  Exe,
  AppsFolder,
  Shell,
};

enum class HitType {
  Result,
  Gear,
  Back,
  CloseSettings,
  RecordShortcut,
  SaveShortcut,
  ClearShortcut,
  CompactToggle,
  AnimationsToggle,
  AccentToggle,
  AccentColor,
  StartupToggle,
  UpdateChecksToggle,
  ShowWindowsToggle,
  ShowStoreAppsToggle,
  ClipboardHistoryToggle,
  ClipboardLimitDown,
  ClipboardLimitUp,
  FileIndexToggle,
  FileIndexLimitDown,
  FileIndexLimitUp,
  AddFileRoot,
  ClearFileRoots,
  DiagnosticsToggle,
  ClearClipboardData,
  ClearFileIndexData,
  OpenLocalDataFolder,
  ReloadExtensions,
  OpenPluginsFolder,
  ClearRecents,
  ClearIconCache,
  CheckUpdates,
  OverlayWidthDown,
  OverlayWidthUp,
  MaxResultsDown,
  MaxResultsUp,
};

enum class CommandKind {
  Settings,
  Quit,
  Restart,
  RefreshApps,
  ClearIconCache,
  ClearRecents,
  OpenDataFolder,
  OpenLocalDataFolder,
  ReloadExtensions,
  LockPC,
  SleepPC,
  MuteAudio,
  ShutDown,
  RestartPC,
  EmptyRecycleBin,
  ClearClipboardHistory,
  OpenSnippetsFile,
  ReloadSnippets,
  OpenThemeFile,
  ReloadTheme,
  CheckForUpdates,
  ClipboardHistory,
  EmojiPicker,
};

// Dedicated browse sub-views entered from a command (Raycast-style): the result
// list is replaced with a single category that the live query filters within.
enum class BrowseView {
  None,
  Clipboard,
  Emoji,
};

enum class ActionKind {
  None,
  Open,
  RunAsAdmin,
  OpenLocation,
  CopyPath,
  Pin,
  Unpin,
  Hide,
  Unhide,
  Switch,
  Minimize,
  MaximizeRestore,
  CloseWindow,
};

struct RectF {
  float left = 0;
  float top = 0;
  float right = 0;
  float bottom = 0;
};

struct HitTarget {
  RectF rect;
  HitType type = HitType::Result;
  int index = -1;
};

// Settings model and JSON round-trip live in settings.hpp (unit-tested there).
using Quicklink = feathercast::settings::Quicklink;
using Settings = feathercast::settings::Settings;
using feathercast::settings::JsonEscape;

struct ShortcutInfo {
  std::wstring target;
  std::wstring args;
  std::wstring cwd;
  std::wstring iconPath;
  int iconIndex = 0;
};

struct AppEntry {
  std::wstring id;
  std::wstring name;
  std::wstring path;
  std::wstring source;
  LaunchType launchType = LaunchType::Shortcut;
  std::wstring launchTarget;
  std::wstring targetPath;
  std::wstring args;
  std::wstring cwd;
  std::wstring appUserModelId;
  std::wstring iconKey;
  bool adminSupported = false;
  bool systemEssential = false;
  bool fileIsDirectory = false;
  long long fileLastWriteTime = 0;
  long long fileSize = 0;
  long long fileIndexedAt = 0;
  std::vector<std::wstring> keywords;
};

struct WindowEntry {
  DWORD pid = 0;
  HWND hwnd = nullptr;
  std::wstring name;
  std::wstring exe;
  std::wstring processName;
  std::wstring iconKey;
};

struct ClipboardEntry {
  std::wstring id;
  std::wstring text;
  std::wstring preview;
  long long capturedAt = 0;
};

// Cached foreign-exchange rates: code -> units per 1 USD (USD itself is 1.0).
struct CurrencyRates {
  std::map<std::wstring, double> perUsd;
  long long fetchedAt = 0;  // unix seconds the table was last refreshed
};

struct DisplayItem {
  bool isWindow = false;
  bool isCommand = false;
  bool isAction = false;
  bool isCalculator = false;
  bool isConversion = false;
  bool isWebSearch = false;
  bool isExtension = false;
  bool isSnippet = false;
  bool isClipboard = false;
  bool isRunCommand = false;
  bool isSymbol = false;
  AppEntry app;
  WindowEntry window;
  feathercast::extensions::QueryResultItem extension;
  feathercast::snippets::Snippet snippet;
  ClipboardEntry clipboard;
  feathercast::run_command::Command runCommand;
  feathercast::symbols::Symbol symbol;
  CommandKind command = CommandKind::Settings;
  ActionKind action = ActionKind::None;
  bool actionTargetIsWindow = false;
  AppEntry actionApp;
  WindowEntry actionWindow;
  std::wstring commandName;
  std::wstring commandDetail;
  std::vector<std::wstring> commandKeywords;
  std::wstring calculationExpression;
  std::wstring calculationResult;
  std::wstring webSearchUrl;
  std::wstring webSearchLabel;

  std::wstring Key() const {
    if (isCalculator) return L"calc:" + calculationExpression;
    if (isConversion) return L"conv:" + calculationExpression;
    if (isWebSearch) return L"web:" + webSearchUrl;
    if (isExtension) return L"ext:" + extension.pluginId + L":" + extension.id;
    if (isSnippet) return L"snippet:" + snippet.keyword;
    if (isClipboard) return L"clip:" + clipboard.id;
    if (isRunCommand) return L"run:" + std::to_wstring(static_cast<int>(runCommand.kind)) + L":" + runCommand.target;
    if (isSymbol) return L"symbol:" + symbol.value;
    if (isCommand) return L"cmd:" + std::to_wstring(static_cast<int>(command));
    if (isAction) return L"act:" + std::to_wstring(static_cast<int>(action)) + L":" +
                         (actionTargetIsWindow ? std::to_wstring(reinterpret_cast<uintptr_t>(actionWindow.hwnd))
                                               : (!actionApp.id.empty() ? actionApp.id : actionApp.path));
    if (isWindow) return L"win:" + std::to_wstring(reinterpret_cast<uintptr_t>(window.hwnd));
    return !app.id.empty() ? app.id : app.path;
  }

  std::wstring Name() const {
    if (isCalculator || isConversion) return calculationResult;
    if (isWebSearch) return webSearchLabel;
    if (isExtension) return extension.title;
    if (isSnippet) return snippet.name;
    if (isClipboard) return clipboard.preview;
    if (isRunCommand) return runCommand.label;
    if (isSymbol) return symbol.label;
    if (isCommand) return commandName;
    if (isAction) return commandName;
    return isWindow ? window.name : app.name;
  }

  std::wstring IconKey() const {
    if (isCalculator || isConversion || isWebSearch || isRunCommand || isSymbol) return L"";
    if (isExtension) return extension.iconPath;
    if (isSnippet || isClipboard) return L"";
    if (isAction) return actionTargetIsWindow ? (!actionWindow.iconKey.empty() ? actionWindow.iconKey : actionWindow.exe)
                                              : (!actionApp.iconKey.empty() ? actionApp.iconKey : actionApp.path);
    if (isCommand) return L"";
    return isWindow ? (!window.iconKey.empty() ? window.iconKey : window.exe)
                    : (!app.iconKey.empty() ? app.iconKey : app.path);
  }
};

struct Section {
  std::wstring title;
  std::vector<DisplayItem> items;
};

// Query-independent search corpus. This is the expensive part to build (it deep-
// copies every app/window/file into DisplayItems and derives a SearchItem for
// each), but it only changes when the underlying data or settings change, never
// when the query text changes. It is therefore built once per data/settings
// version (BuildSnapshot) and shared across keystrokes via shared_ptr<const>, so
// typing fast no longer rebuilds the corpus on every character. Immutable once
// built, so the search worker can read it without locking.
struct SearchSnapshot {
  // Non-empty search path (pool is parallel to searchItems).
  std::vector<DisplayItem> pool;
  std::vector<feathercast::core::PreparedSearchItem> searchItems;

  // Empty-query path (pre-bucketed; needs settings_).
  std::vector<DisplayItem> pinned;
  std::vector<DisplayItem> recent;
  std::vector<DisplayItem> windowItems;
  std::vector<DisplayItem> system;
  std::vector<DisplayItem> systemFolders;
  std::vector<DisplayItem> commandItems;
  std::vector<DisplayItem> snippetItems;
  std::vector<DisplayItem> clipboardItems;

  // Clipboard browse-view path (parallel to clipboardItems).
  std::vector<feathercast::core::SearchItem> clipboardSearchItems;
};

struct SnapshotBuildRequest {
  uint64_t revision = 0;
  Settings settings;
};

struct SnapshotBuildResult {
  uint64_t revision = 0;
  std::shared_ptr<const SearchSnapshot> snapshot;
};

struct DiscoveryRequest {
  uint64_t generation = 0;
  bool fileIndexEnabled = false;
  size_t fileIndexLimit = 0;
  std::vector<std::wstring> configuredRoots;
};

// Self-contained snapshot of everything the search engine needs to produce a
// result set. The heavy, query-independent corpus is referenced through a shared
// SearchSnapshot; the remaining fields are cheap and query/mode specific, so they
// are gathered on the UI thread per request. Handed to ComputeResults, which is
// pure and may run on a worker thread.
struct QueryRequest {
  unsigned long long generation = 0;
  std::wstring query;
  bool empty = false;
  bool actionMode = false;
  BrowseView browseView = BrowseView::None;  // dedicated clipboard/emoji sub-view
  bool compactClear = false;  // compact mode + empty query: render nothing
  int limit = 0;              // already clamped from settings_.maxResults
  long long now = 0;          // shared clock for usage/recency scoring
  const std::atomic<unsigned long long>* latestGeneration = nullptr;
  std::set<std::wstring> recentIds;
  std::map<std::wstring, std::wstring> searchEngines;   // web search prefixes
  std::map<std::wstring, double> currencyRates;          // code -> rate per USD
  std::wstring defaultCurrency;                          // locale currency for "USD 5"

  // Cached, query-independent corpus shared across keystrokes.
  std::shared_ptr<const SearchSnapshot> snapshot;

  // Query-dependent extension results (from the extension result cache).
  std::vector<DisplayItem> extensionItems;

  // Action-mode path (target-specific; built per request).
  std::vector<DisplayItem> actions;
  std::vector<feathercast::core::SearchItem> actionSearchItems;
};

struct ResultsCollection {
  unsigned long long generation = 0;
  std::vector<Section> sections;
  std::vector<DisplayItem> flatItems;
};

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

DisplayItem CommandDisplay(CommandKind command, std::wstring name, std::wstring detail, std::vector<std::wstring> keywords) {
  DisplayItem item;
  item.isCommand = true;
  item.command = command;
  item.commandName = std::move(name);
  item.commandDetail = std::move(detail);
  item.commandKeywords = std::move(keywords);
  return item;
}

DisplayItem ActionDisplay(ActionKind action, std::wstring name, std::wstring detail, const DisplayItem& target) {
  DisplayItem item;
  item.isAction = true;
  item.action = action;
  item.commandKeywords = {name, detail};
  item.commandName = std::move(name);
  item.commandDetail = std::move(detail);
  item.actionTargetIsWindow = target.isWindow;
  item.actionApp = target.app;
  item.actionWindow = target.window;
  return item;
}

DisplayItem CalculatorDisplay(const feathercast::calculator::Result& result) {
  DisplayItem item;
  item.isCalculator = true;
  item.calculationExpression = result.expression;
  item.calculationResult = result.display;
  item.commandDetail = L"Calculator result - " + result.expression;
  item.commandKeywords = {L"calculator", L"calc", result.expression, result.display};
  return item;
}

DisplayItem ConversionDisplay(const std::wstring& expression, const std::wstring& display) {
  DisplayItem item;
  item.isConversion = true;
  item.calculationExpression = expression;
  item.calculationResult = display;
  item.commandDetail = L"Conversion - " + expression;
  item.commandKeywords = {L"convert", L"conversion", expression, display};
  return item;
}

// Percent-encodes a query for use in a URL (UTF-8 octets, RFC 3986 unreserved set).
std::string UrlEncode(const std::wstring& text) {
  static const char* kHex = "0123456789ABCDEF";
  const std::string utf8 = WideToUtf8(text);
  std::string out;
  for (const unsigned char ch : utf8) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else if (ch == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[ch >> 4]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

DisplayItem WebSearchDisplay(const std::wstring& keyword, const std::wstring& engineTemplate, const std::wstring& terms) {
  std::wstring url = engineTemplate;
  const std::wstring encoded = Utf8ToWide(UrlEncode(terms));
  if (const size_t pos = url.find(L"%s"); pos != std::wstring::npos) {
    url.replace(pos, 2, encoded);
  } else {
    url += encoded;
  }
  DisplayItem item;
  item.isWebSearch = true;
  item.webSearchUrl = url;
  item.webSearchLabel = L"Search " + keyword + L" for \"" + terms + L"\"";
  item.commandDetail = url;
  item.commandKeywords = {L"web", L"search", keyword, terms};
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

DisplayItem RunCommandDisplay(const feathercast::run_command::Command& command) {
  DisplayItem item;
  item.isRunCommand = true;
  item.runCommand = command;
  item.commandDetail = command.detail;
  item.commandKeywords = {L"run", L"command", L"shell", L"url", command.input, command.target};
  return item;
}

DisplayItem SymbolDisplay(const feathercast::symbols::Symbol& symbol) {
  DisplayItem item;
  item.isSymbol = true;
  item.symbol = symbol;
  item.commandDetail = L"Symbol - " + symbol.value;
  item.commandKeywords = symbol.keywords;
  item.commandKeywords.push_back(symbol.label);
  item.commandKeywords.push_back(symbol.value);
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

// Opt-in diagnostics for launch troubleshooting. Paths can contain personal
// information, so release builds never create this log unless enabled.
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

std::vector<feathercast::snippets::Snippet> LoadSnippets() {
  std::ifstream file(SnippetsPath(), std::ios::binary);
  if (!file) return {};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return feathercast::snippets::ParseSnippetsJson(buffer.str());
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

// Performs a simple HTTPS GET and returns the response body, or nullopt on error.
std::optional<std::string> HttpsGet(const std::wstring& host, const std::wstring& path) {
  std::optional<std::string> result;
  HINTERNET session = WinHttpOpen(L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return result;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

  HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) return result;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });

  HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
  if (!request) return result;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });

  // The GitHub REST API requires an Accept header and a pinned API version; the
  // User-Agent is already supplied via WinHttpOpen above.
  static constexpr wchar_t kGitHubHeaders[] =
      L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28";
  WinHttpAddRequestHeaders(request, kGitHubHeaders, static_cast<DWORD>(-1),
                           WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    return result;
  }

  // Treat any non-2xx response (e.g. 403 rate limit, 404 missing release) as a
  // reachability failure so the caller surfaces a clear network error instead of
  // trying to parse an error payload as release metadata.
  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
      (status < 200 || status >= 300)) {
    return result;
  }

  std::string body;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
    chunk.resize(read);
    body += chunk;
    if (body.size() > 2 * 1024 * 1024) break;  // sanity cap
  }
  result = std::move(body);
  return result;
}

struct HttpsUrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

std::optional<HttpsUrlParts> ParseHttpsUrl(const std::wstring& url) {
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);

  std::wstring mutableUrl = url;
  if (!WinHttpCrackUrl(mutableUrl.c_str(), 0, 0, &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS ||
      !components.lpszHostName || components.dwHostNameLength == 0) {
    return std::nullopt;
  }

  HttpsUrlParts parts;
  parts.host.assign(components.lpszHostName, components.dwHostNameLength);
  if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
    parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
    parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (parts.path.empty()) parts.path = L"/";
  parts.port = components.nPort ? components.nPort : INTERNET_DEFAULT_HTTPS_PORT;
  return parts;
}

std::optional<std::string> HttpsGetUrl(const std::wstring& url, size_t maxBytes = 2 * 1024 * 1024) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts) return std::nullopt;

  HINTERNET session = WinHttpOpen(L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return std::nullopt;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

  HINTERNET connect = WinHttpConnect(session, parts->host.c_str(), parts->port, 0);
  if (!connect) return std::nullopt;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });

  HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts->path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
  if (!request) return std::nullopt;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    return std::nullopt;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
      (status < 200 || status >= 300)) {
    return std::nullopt;
  }

  std::string body;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    if (body.size() + available > maxBytes) return std::nullopt;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) return std::nullopt;
    chunk.resize(read);
    body += chunk;
  }
  return body;
}

bool HttpsDownloadToFile(const std::wstring& url, const std::filesystem::path& destination,
                         std::stop_token stopToken, size_t maxBytes = 250 * 1024 * 1024) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts) return false;

  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  auto temp = destination;
  temp += L".tmp";
  std::filesystem::remove(temp, ec);

  HINTERNET session = WinHttpOpen(L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

  HINTERNET connect = WinHttpConnect(session, parts->host.c_str(), parts->port, 0);
  if (!connect) return false;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });

  HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts->path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
  if (!request) return false;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    return false;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) &&
      (status < 200 || status >= 300)) {
    return false;
  }

  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file) return false;

  size_t total = 0;
  DWORD available = 0;
  while (!stopToken.stop_requested() && WinHttpQueryDataAvailable(request, &available) && available > 0) {
    if (total + available > maxBytes) return false;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) return false;
    if (read == 0) continue;
    file.write(chunk.data(), static_cast<std::streamsize>(read));
    if (!file) return false;
    total += read;
  }
  file.close();
  if (stopToken.stop_requested() || total == 0) {
    std::filesystem::remove(temp, ec);
    return false;
  }

  if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(destination, ec);
    std::filesystem::rename(temp, destination, ec);
  }
  const bool ok = std::filesystem::exists(destination, ec);
  if (!ok) std::filesystem::remove(temp, ec);
  return ok;
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

bool ToggleDefaultAudioMute() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(enumerator.GetAddressOf())))) {
    return false;
  }

  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf()))) {
    return false;
  }

  ComPtr<IAudioEndpointVolume> volume;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(volume.GetAddressOf())))) {
    return false;
  }

  BOOL muted = FALSE;
  if (FAILED(volume->GetMute(&muted))) return false;
  return SUCCEEDED(volume->SetMute(!muted, nullptr));
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

Settings LoadSettings() {
  Settings settings;
  std::ifstream file(SettingsPath(), std::ios::binary);
  if (!file) return settings;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  settings = feathercast::settings::ParseSettings(buffer.str());
  settings.overlayWidth = std::clamp(settings.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
  settings.maxResults = std::clamp(settings.maxResults, MIN_RESULTS, MAX_RESULT_SETTING);
  settings.clipboardHistoryLimit =
      std::clamp(settings.clipboardHistoryLimit, MIN_CLIPBOARD_HISTORY_LIMIT, MAX_CLIPBOARD_HISTORY_LIMIT);
  settings.fileIndexMaxEntries =
      std::clamp(settings.fileIndexMaxEntries, MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
  return settings;
}

bool SaveSettings(const Settings& settings) {
  const auto path = SettingsPath();
  auto temp = path;
  temp += L".tmp";
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  Settings clamped = settings;
  clamped.overlayWidth = std::clamp(clamped.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
  clamped.maxResults = std::clamp(clamped.maxResults, MIN_RESULTS, MAX_RESULT_SETTING);
  clamped.clipboardHistoryLimit =
      std::clamp(clamped.clipboardHistoryLimit, MIN_CLIPBOARD_HISTORY_LIMIT, MAX_CLIPBOARD_HISTORY_LIMIT);
  clamped.fileIndexMaxEntries =
      std::clamp(clamped.fileIndexMaxEntries, MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
  file << feathercast::settings::SerializeSettings(clamped);
  file.close();
  if (!file) {
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    return false;
  }

  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
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

// Disables DWM's own rounded corners so the Direct2D panel is the only visible shape.
inline void DisableDwmRoundedCorners(HWND hwnd) {
  constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
  DWORD preference = 1;  // DWMWCP_DONOTROUND
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

// Undocumented (but stable since Win10 1803) SetWindowCompositionAttribute accent
// policy. FeatherCast explicitly disables this material path so Windows' global
// transparency setting cannot turn the popup background into an opaque Acrylic/Mica fill.
enum class AccentState : DWORD {
  Disabled = 0,
  EnableAcrylicBlurBehind = 4,
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

inline void ApplyAccentPolicy(HWND hwnd, AccentState state, DWORD gradientColor = 0) {
  using PfnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WindowCompositionAttribData*);
  static PfnSetWindowCompositionAttribute setAttr = []() -> PfnSetWindowCompositionAttribute {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      return reinterpret_cast<PfnSetWindowCompositionAttribute>(
          GetProcAddress(user32, "SetWindowCompositionAttribute"));
    }
    return nullptr;
  }();
  if (!setAttr) return;
  AccentPolicy policy{state, 0, gradientColor, 0};
  WindowCompositionAttribData data{19 /*WCA_ACCENT_POLICY*/, &policy, sizeof(policy)};
  setAttr(hwnd, &data);
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

void SendVirtualKeyTap(UINT vk) {
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = static_cast<WORD>(vk);
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = static_cast<WORD>(vk);
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(static_cast<UINT>(sizeof(inputs) / sizeof(inputs[0])), inputs, sizeof(INPUT));
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

struct ExtensionActivationResult {
  std::optional<feathercast::extensions::ActivationResponse> response;
};

struct LaunchCompletion {
  std::wstring id;
  std::wstring name;
  bool succeeded = false;
};

struct NavigationState {
  View view = View::Search;
  bool actionMode = false;
  BrowseView browseView = BrowseView::None;
  std::wstring query;
  size_t caret = 0;
  std::optional<size_t> selectionAnchor;
  int selected = 0;
  int scroll = 0;
  std::wstring selectedKey;
};

class FeatherCastApp;
FeatherCastApp* g_app = nullptr;

class FeatherCastApp : public feathercast::accessibility::Model {
 public:
  explicit FeatherCastApp(HINSTANCE instance, std::wstring cmdLine)
      : instance_(instance), cmdLine_(std::move(cmdLine)) {
    QueryPerformanceFrequency(&qpcFrequency_);
    settings_ = LoadSettings();
    g_diagnosticsEnabled = settings_.diagnosticsEnabled;
    hadLegacyOperationalData_ = MigrateLegacyOperationalData();
    shortcut_ = ParseShortcut(settings_.shortcut);
    theme_ = feathercast::theme::LoadTheme(ThemePath());
    RefreshSystemPreferences();
    snippets_ = LoadSnippets();
    systemFolders_ = SystemFolderEntries();
    if (!SetStartOnStartup(settings_.startOnStartup) && settings_.startOnStartup) {
      settings_.startOnStartup = false;
      SaveSettings(settings_);
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
    launchExecutor_.Shutdown();
    extensions_.Shutdown();
    searchCv_.notify_all();
    snapshotCv_.notify_all();
    discoveryCv_.notify_all();
    if (searchThread_.joinable()) {
      searchThread_.request_stop();
      searchThread_.join();
    }
    if (snapshotThread_.joinable()) {
      snapshotThread_.request_stop();
      snapshotThread_.join();
    }
    if (discoveryThread_.joinable()) {
      discoveryThread_.request_stop();
      discoveryThread_.join();
    }
    persistenceExecutor_.Shutdown(true);
    if (currencyThread_.joinable()) {
      currencyThread_.request_stop();
      currencyThread_.join();
    }
    if (updateThread_.joinable()) {
      updateThread_.request_stop();
      updateThread_.join();
    }
    StopIconThreads();
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
    if (!InitializeFactories() || !RegisterWindowClass() || !CreateMainWindow() || !CreateSettingsWindow()) {
      MessageBoxW(nullptr, L"FeatherCast could not initialize its native windows or graphics factories.",
                  L"FeatherCast Startup", MB_OK | MB_ICONERROR);
      ReleaseComResources();
      CoUninitialize();
      g_app = nullptr;
      return 1;
    }
    PromptForPrivacyConsentIfNeeded();
    LoadPersistentState();
    UpdateClipboardListenerRegistration();
    if (!CreateTray()) {
      MessageBoxW(hwnd_, L"FeatherCast could not create its tray icon.",
                  L"FeatherCast Startup", MB_OK | MB_ICONWARNING);
    }
    extensions_.Initialize(UserDataPath(), ExeDirectory(), hwnd_, WM_REBUILD_RESULTS);
    const bool hotKeyReady = RegisterShortcutHotKey();
    const bool hookReady = InstallHook();
    if (!hotKeyReady && !hookReady) {
      MessageBoxW(hwnd_, L"FeatherCast could not register the global shortcut or keyboard hook.",
                  L"FeatherCast Startup", MB_OK | MB_ICONWARNING);
    }
    launchExecutor_.Start(2);
    persistenceExecutor_.Start(1);
    StartSearchWorker();
    StartSnapshotWorker();
    StartDiscoveryWorker();
    StartIconWorkers();
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
        PostMessageW(hwnd_, WM_SHORTCUT_TOGGLE, result.suppressWinStart && shortcut_.singleModifier ? 1 : 0, 0);
      }
      if (result.consume) return 1;
    }
    return CallNextHookEx(hook_, nCode, wParam, lParam);
  }

  std::wstring AccessibleWindowName(HWND hwnd) const override {
    return hwnd == settingsHwnd_ ? L"FeatherCast Settings" : L"FeatherCast Launcher";
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

    if (hwnd == hwnd_) {
      Item search;
      search.name = L"Search";
      search.value = query_;
      search.description = std::to_wstring(flatItems_.size()) + L" results";
      search.role = ROLE_SYSTEM_TEXT;
      search.state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED;
      search.screenRect = screenRect({52, 12, static_cast<float>(client.right) / scale - 94, 50});
      items.push_back(std::move(search));

      float y = kResultsTop - static_cast<float>(scroll_);
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
          if (index == selected_) result.state |= STATE_SYSTEM_SELECTED | STATE_SYSTEM_FOCUSED;
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
      switch (type) {
        case HitType::RecordShortcut: return L"Record global shortcut";
        case HitType::SaveShortcut: return L"Save global shortcut";
        case HitType::ClearShortcut: return L"Clear global shortcut";
        case HitType::StartupToggle: return L"Start on startup";
        case HitType::UpdateChecksToggle: return L"Automatic update checks";
        case HitType::CompactToggle: return L"Compact mode";
        case HitType::AnimationsToggle: return L"Enable animations";
        case HitType::ShowWindowsToggle: return L"Open window results";
        case HitType::ShowStoreAppsToggle: return L"Store and system apps";
        case HitType::OverlayWidthDown: return L"Decrease overlay width";
        case HitType::OverlayWidthUp: return L"Increase overlay width";
        case HitType::MaxResultsDown: return L"Decrease maximum results";
        case HitType::MaxResultsUp: return L"Increase maximum results";
        case HitType::ClipboardHistoryToggle: return L"Clipboard history";
        case HitType::ClipboardLimitDown: return L"Decrease clipboard history retention";
        case HitType::ClipboardLimitUp: return L"Increase clipboard history retention";
        case HitType::FileIndexToggle: return L"Files and folders index";
        case HitType::FileIndexLimitDown: return L"Decrease file index limit";
        case HitType::FileIndexLimitUp: return L"Increase file index limit";
        case HitType::AddFileRoot: return L"Add file index folder";
        case HitType::ClearFileRoots: return L"Use default file index folders";
        case HitType::DiagnosticsToggle: return L"Enable diagnostics";
        case HitType::ClearClipboardData: return L"Delete clipboard data";
        case HitType::ClearFileIndexData: return L"Delete file index";
        case HitType::OpenLocalDataFolder: return L"Open local data folder";
        case HitType::ReloadExtensions: return L"Reload extensions";
        case HitType::OpenPluginsFolder: return L"Open plugins folder";
        case HitType::AccentToggle: return L"Sync accent color";
        case HitType::AccentColor: return L"Pick accent color";
        case HitType::ClearRecents: return L"Clear recents";
        case HitType::ClearIconCache: return L"Clear icon cache";
        case HitType::CheckUpdates: return L"Check for updates";
        case HitType::CloseSettings: return L"Close settings";
        default: return L"Setting";
      }
    };
    auto checked = [&](HitType type) {
      switch (type) {
        case HitType::StartupToggle: return settings_.startOnStartup;
        case HitType::UpdateChecksToggle: return settings_.updateChecksEnabled;
        case HitType::CompactToggle: return settings_.compactMode;
        case HitType::AnimationsToggle: return settings_.animationsEnabled;
        case HitType::ShowWindowsToggle: return settings_.showOpenWindows;
        case HitType::ShowStoreAppsToggle: return settings_.showStoreApps;
        case HitType::ClipboardHistoryToggle: return settings_.clipboardHistoryEnabled;
        case HitType::FileIndexToggle: return settings_.fileIndexEnabled;
        case HitType::DiagnosticsToggle: return settings_.diagnosticsEnabled;
        case HitType::AccentToggle: return settings_.syncAccentColor;
        default: return false;
      }
    };
    for (const HitType type : order) {
      const auto hit = std::find_if(hits_.begin(), hits_.end(),
                                    [&](const HitTarget& target) { return target.type == type; });
      if (hit == hits_.end()) continue;
      Item setting;
      setting.name = nameFor(type);
      setting.defaultAction = L"Activate";
      setting.role = type == HitType::StartupToggle || type == HitType::UpdateChecksToggle ||
                             type == HitType::CompactToggle || type == HitType::AnimationsToggle ||
                             type == HitType::ShowWindowsToggle || type == HitType::ShowStoreAppsToggle ||
                             type == HitType::ClipboardHistoryToggle || type == HitType::FileIndexToggle ||
                             type == HitType::DiagnosticsToggle || type == HitType::AccentToggle
                         ? ROLE_SYSTEM_CHECKBUTTON
                         : ROLE_SYSTEM_PUSHBUTTON;
      setting.state = STATE_SYSTEM_FOCUSABLE;
      if (checked(type)) setting.state |= STATE_SYSTEM_CHECKED;
      setting.screenRect = screenRect(hit->rect);
      items.push_back(std::move(setting));
    }
    return items;
  }

  int AccessibleFocusedChild(HWND hwnd) const override {
    if (hwnd == hwnd_) return flatItems_.empty() ? 1 : selected_ + 2;
    return settingsFocusIndex_ + 1;
  }

  void AccessibleFocusChild(HWND hwnd, int child) override {
    if (hwnd == hwnd_) {
      if (child <= 1) {
        SetFocus(hwnd_);
      } else {
        SelectResult(child - 2, false, true);
      }
      return;
    }
    const auto order = SettingsFocusOrder();
    settingsFocusIndex_ = std::clamp(child - 1, 0, static_cast<int>(order.size()) - 1);
    EnsureSettingsFocusVisible();
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void AccessibleInvokeChild(HWND hwnd, int child) override {
    if (hwnd == hwnd_) {
      const int index = child - 2;
      if (index >= 0 && index < static_cast<int>(flatItems_.size())) Activate(flatItems_[index], false);
      return;
    }
    const auto order = SettingsFocusOrder();
    const int index = child - 1;
    if (index < 0 || index >= static_cast<int>(order.size())) return;
    if (order[static_cast<size_t>(index)] == HitType::CloseSettings) HideSettings();
    else HandleSettingsHit(order[static_cast<size_t>(index)]);
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
    if (taskbarCreatedMessage_ && msg == taskbarCreatedMessage_) {
      RemoveTray();
      CreateTray();
      return 0;
    }
    switch (msg) {
      case WM_TIMER:
        if (wParam == 1) {
          if (visible_) {
            if (!suppressHide_) {
              HWND foreground = GetForegroundWindow();
              if (foreground && foreground != hwnd_) {
                HideOverlay(false);
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
        } else if (wParam == TIMER_SELECTION_ANIM) {
          OnSelectionAnimationTimer();
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
          if (!recording_) TriggerShortcutToggle();
          return 0;
        }
        break;
      case WM_SHORTCUT_TOGGLE:
        if (wParam) ReleaseWinModifierState();
        if (!recording_) TriggerShortcutToggle();
        return 0;
      case WM_DESTROY:
        UnregisterShortcutHotKey();
        if (settingsHwnd_) {
          HWND settings = settingsHwnd_;
          settingsHwnd_ = nullptr;
          DestroyWindow(settings);
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
        if (overlaySurface_.dc) {
          float dpi = static_cast<float>(LOWORD(wParam));
          overlaySurface_.dc->SetDpi(dpi, dpi);
        }
        ApplyRoundedRegion(prc->right - prc->left, prc->bottom - prc->top);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && visible_ && !suppressHide_) HideOverlay(false);
        return 0;
      case WM_KILLFOCUS:
        if (visible_ && !suppressHide_) HideOverlay(false);
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
        if (gearHovered_) {
          gearHovered_ = false;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      case WM_LBUTTONDOWN: {
        const float scale = GetWindowScale(hwnd_);
        OnClick(static_cast<float>(GET_X_LPARAM(lParam)) / scale, static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_RBUTTONUP: {
        const float scale = GetWindowScale(hwnd_);
        OnRightClick(static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                     static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
      case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
      case WM_TRAYICON:
        OnTray(lParam);
        return 0;
      case WM_SHOW_SEARCH:
        ShowOverlay(View::Search);
        return 0;
      case WM_ICON_READY:
        iconReadyMessageQueued_.store(false, std::memory_order_release);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_REBUILD_RESULTS:
        // Data-mutation sites (apps_/windows_/fileIndex_/snippets_/clipboard) and
        // Settings/data changes advance the corpus revision themselves; this also fires for
        // async extension results, which do not affect the cached corpus.
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      case WM_SEARCH_READY: {
        ResultsCollection result;
        {
          std::lock_guard lock(searchMutex_);
          result = std::move(latestResult_);
        }
        if (result.generation == searchGeneration_) {
          ApplyResults(std::move(result));
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      }
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
      case WM_UPDATE_READY:
        OnUpdateReady();
        return 0;
      case WM_EXTENSION_ACTIVATED:
        OnExtensionActivationReady();
        return 0;
      case WM_SNAPSHOT_READY:
        OnSnapshotReady();
        return 0;
      case WM_PERSISTENCE_ERROR:
        persistenceErrorQueued_.store(false, std::memory_order_release);
        if (settingsHwnd_ && IsWindowVisible(settingsHwnd_)) {
          MessageBoxW(settingsHwnd_,
                      L"FeatherCast could not save local changes. Check disk access and available space.",
                      L"FeatherCast Storage", MB_OK | MB_ICONWARNING);
        } else {
          ShowTrayNotification(L"FeatherCast Storage",
                               L"Some local changes could not be saved.");
        }
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
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const float scale = GetWindowScale(hwnd);
        info->ptMinTrackSize.x = static_cast<LONG>(520.0f * scale);
        info->ptMinTrackSize.y = static_cast<LONG>(400.0f * scale);
        return 0;
      }
      case WM_DPICHANGED: {
        auto prc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (settingsSurface_.dc) {
          float dpi = static_cast<float>(LOWORD(wParam));
          settingsSurface_.dc->SetDpi(dpi, dpi);
        }
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
        const float lheight = static_cast<float>(rc.bottom - rc.top) / scale;
        constexpr float border = 8.0f;
        const bool left = lx < border;
        const bool right = lx >= lwidth - border;
        const bool top = ly < border;
        const bool bottom = ly >= lheight - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        const bool overClose = lx >= lwidth - 46.0f && lx <= lwidth - 14.0f && ly >= 12.0f && ly <= 44.0f;
        if (ly >= 0.0f && ly < 52.0f && !overClose) return HTCAPTION;
        return HTCLIENT;
      }
      case WM_KEYDOWN:
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
        const float scale = GetWindowScale(hwnd);
        OnSettingsClick(static_cast<float>(GET_X_LPARAM(lParam)) / scale, static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        return 0;
      }
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
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }

  // (Re)applies the transparent window treatment. The visible background is drawn into
  // a premultiplied DirectComposition swap chain; DWM Acrylic/Mica materials are disabled
  // so the popup stays see-through even when Windows transparency effects are off.
  void ApplyGlass(HWND hwnd) {
    ApplyDarkMode(hwnd);
    MARGINS margins{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    ApplyModernBackdrop(hwnd, DwmBackdropType::None);
    ApplyAccentPolicy(hwnd, AccentState::Disabled);
    DisableDwmRoundedCorners(hwnd);
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
    // Transparent popup: DirectComposition supplies per-pixel alpha; DWM materials stay off.
    ApplyGlass(hwnd_);
    return true;
  }

  bool CreateSettingsWindow() {
    const int height = SettingsContentHeight();
    settingsHwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
      kSettingsWindowClass,
      L"FeatherCast Settings",
      WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX,
      -32000,
      -32000,
      SETTINGS_WIDTH,
      height,
      nullptr,
      nullptr,
      instance_,
      this);

    if (!settingsHwnd_) return false;
    // Settings uses the same transparent DirectComposition background as the launcher.
    ApplyGlass(settingsHwnd_);
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

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   d3dDevice_.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr)) {
      // Fall back to WARP so the app still renders on machines without a usable GPU path.
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
    dcompDevice_.Reset();
    dxgiFactory_.Reset();
    d2dDevice_.Reset();
    d3dDevice_.Reset();
    ClearIconBitmaps();
    brushCache_.clear();
  }

  // (Re)binds the swap chain's back buffer as the Direct2D device context's target.
  bool BindSurfaceTarget(GlassSurface& surface, float dpi) {
    ComPtr<IDXGISurface> dxgiSurface;
    if (FAILED(surface.swapChain->GetBuffer(0, IID_PPV_ARGS(dxgiSurface.GetAddressOf())))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    surface.bitmap.Reset();
    if (FAILED(surface.dc->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, surface.bitmap.GetAddressOf()))) return false;
    surface.dc->SetTarget(surface.bitmap.Get());
    surface.dc->SetDpi(dpi, dpi);
    return true;
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

    if (!BindSurfaceTarget(surface, dpi)) {
      surface.Reset();
      return false;
    }
    EnsureTextFormats();
    return true;
  }

  // Resizes a surface's swap chain to match the window's new physical client size.
  void ResizeGlassSurface(GlassSurface& surface, HWND hwnd, UINT width, UINT height) {
    if (!surface.swapChain || !surface.dc) return;
    width = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    surface.dc->SetTarget(nullptr);
    surface.bitmap.Reset();
    if (FAILED(surface.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) return;
    BindSurfaceTarget(surface, GetWindowScale(hwnd) * 96.0f);
  }

  void ResetTextFormats() {
    inputFormat_.Reset();
    rowFormat_.Reset();
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
    ResetTextFormats();
  }

  bool AnimationsAllowed() const {
    return settings_.animationsEnabled && systemAnimationsEnabled_ && !highContrast_;
  }

  void EnsureTextFormats() {
    if (!dwriteFactory_ || inputFormat_) return;
    // 18px Regular: elegant search-bar input (was 19px).
    CreateTextFormat(18.0f, DWRITE_FONT_WEIGHT_NORMAL, inputFormat_);
    // 14px Medium: stronger name/title separation from subtitle (was 15px Normal).
    CreateTextFormat(14.0f, DWRITE_FONT_WEIGHT_MEDIUM, rowFormat_);
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
    centerFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    centerFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    CreateTextFormat(22.0f, DWRITE_FONT_WEIGHT_NORMAL, emojiFormat_);
    if (emojiFormat_) {
      emojiFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      emojiFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    const auto hotKey = ToHotKeySpec(shortcut_);
    if (!hwnd_ || !hotKey.supported) return false;
    hotKeyRegistered_ = RegisterHotKey(hwnd_, HOTKEY_OPEN_SEARCH, hotKey.modifiers, hotKey.vk) != FALSE;
    return hotKeyRegistered_;
  }

  bool CanActivateShortcut(const ShortcutSpec& candidate) const {
    if (candidate.display.empty() || candidate.display == L"none") return true;
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
    if (hwnd_) UnregisterHotKey(hwnd_, HOTKEY_OPEN_SEARCH);
    hotKeyRegistered_ = false;
  }

  LRESULT HandleRecordingKey(UINT vk, bool down, bool up) {
    const auto result = shortcutRecorder_.Handle(vk, down, up);
    if (result.canceled) {
      recording_ = false;
      pendingShortcut_.clear();
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
    } else if (result.done) {
      pendingShortcut_ = result.shortcut;
      recording_ = false;
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
    persistenceExecutor_.Submit([this, limit](std::stop_token stopToken) {
      if (stopToken.stop_requested()) return;
      std::lock_guard lock(storageMutex_);
      if (storage_.IsOpen()) storage_.PruneClipboardHistory(limit);
    });
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

  void ReloadClipboardHistoryFromStorage() {
    std::vector<ClipboardEntry> loaded;
    unsigned long long serial = 0;
    if (!settings_.clipboardHistoryEnabled || !storage_.IsOpen()) {
      std::lock_guard dataLock(dataMutex_);
      clipboardHistory_.clear();
      clipboardSerial_ = 0;
      return;
    }
    for (const auto& entry : storage_.LoadClipboardHistory(ClipboardHistoryLimit())) {
      ClipboardEntry item;
      item.id = std::to_wstring(entry.id);
      item.text = entry.text;
      item.preview = entry.preview;
      item.capturedAt = entry.capturedAt;
      loaded.push_back(std::move(item));
      serial = std::max<unsigned long long>(serial, static_cast<unsigned long long>(entry.id));
    }
    {
      std::lock_guard dataLock(dataMutex_);
      clipboardHistory_ = std::move(loaded);
      clipboardSerial_ = serial;
    }
  }

  void LoadPersistentState() {
    std::lock_guard storageLock(storageMutex_);
    if (!storage_.Open(DatabasePath())) {
      const auto& error = storage_.LastError();
      const std::wstring detail =
          error.message.empty() ? L"Unknown SQLite error." : Utf8ToWide(error.message);
      MessageBoxW(hwnd_,
                  (L"FeatherCast could not open its local database.\n\n" + detail +
                   L"\n\nFile indexing and clipboard persistence will be unavailable.").c_str(),
                  L"FeatherCast Storage", MB_OK | MB_ICONWARNING);
      return;
    }
    if (storage_.RecoveredFromCorruption()) {
      MessageBoxW(
          hwnd_,
          (L"FeatherCast detected a damaged local database and created a new one.\n\n"
           L"The damaged file was preserved at:\n" +
           storage_.QuarantinedPath().wstring()).c_str(),
          L"FeatherCast Storage Recovery", MB_OK | MB_ICONWARNING);
    }

    if (settings_.fileIndexEnabled) {
      for (const auto& entry : storage_.LoadFileIndex(FileIndexLimit())) {
        if (!entry.path.empty() && !entry.name.empty()) fileIndex_.push_back(FileIndexApp(entry));
      }
    }
    ReloadClipboardHistoryFromStorage();
  }

  void ReloadTheme() {
    theme_ = feathercast::theme::LoadTheme(ThemePath());
    RefreshSystemPreferences();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (settingsHwnd_) InvalidateRect(settingsHwnd_, nullptr, FALSE);
  }

  void StartAppDiscovery() {
    const uint64_t generation =
        latestDiscoveryGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    appsReady_.store(false, std::memory_order_release);
    PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);

    DiscoveryRequest request;
    request.generation = generation;
    request.fileIndexEnabled = settings_.fileIndexEnabled;
    request.fileIndexLimit = FileIndexLimit();
    request.configuredRoots = settings_.fileIndexRoots;
    {
      std::lock_guard lock(discoveryMutex_);
      pendingDiscovery_ = std::move(request);
    }
    discoveryCv_.notify_one();
  }

  bool DiscoveryCanceled(std::stop_token stopToken, uint64_t generation) const {
    return stopToken.stop_requested() || stopThreads_ ||
           latestDiscoveryGeneration_.load(std::memory_order_acquire) != generation;
  }

  void StartDiscoveryWorker() {
    discoveryThread_ = std::jthread([this](std::stop_token stopToken) {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      ScopeExit uninitialize([] { CoUninitialize(); });
      for (;;) {
        DiscoveryRequest request;
        {
          std::unique_lock lock(discoveryMutex_);
          discoveryCv_.wait(lock, [&] {
            return pendingDiscovery_.has_value() || stopToken.stop_requested() || stopThreads_;
          });
          if (stopToken.stop_requested() || stopThreads_) return;
          request = std::move(*pendingDiscovery_);
          pendingDiscovery_.reset();
        }

        auto apps = DiscoverApps(stopToken, request.generation);
        if (!apps || DiscoveryCanceled(stopToken, request.generation)) continue;
        {
          std::lock_guard lock(dataMutex_);
          apps_ = std::move(*apps);
        }
        appsReady_.store(true, std::memory_order_release);
        MarkSearchDataChanged();
        if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);

        if (request.fileIndexEnabled) {
          BuildFileIndex(stopToken, request.generation, request.configuredRoots,
                         request.fileIndexLimit);
        } else if (!DiscoveryCanceled(stopToken, request.generation)) {
          {
            std::lock_guard lock(dataMutex_);
            fileIndex_.clear();
          }
          MarkSearchDataChanged();
        }
        if (DiscoveryCanceled(stopToken, request.generation)) continue;
        if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
        PrecacheIcons(stopToken, request.generation);
      }
    });
  }

  static constexpr int kFileIndexDepth = 4;

  void BuildFileIndex(std::stop_token stopToken, uint64_t generation,
                      const std::vector<std::wstring>& configuredRoots, size_t limit) {
    std::vector<AppEntry> files;
    std::set<std::wstring> seen;
    const long long indexedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<std::filesystem::path> roots;
    for (const auto& configuredRoot : configuredRoots) {
      if (!Trim(configuredRoot).empty()) roots.emplace_back(configuredRoot);
    }
    if (roots.empty()) {
      for (const KNOWNFOLDERID& folder : {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads}) {
        const std::wstring root = KnownFolderPath(folder);
        if (!root.empty()) roots.emplace_back(root);
      }
    }

    struct RootState {
      std::deque<std::pair<std::filesystem::path, int>> directories;
      std::deque<std::pair<std::filesystem::path, int>> entries;
    };
    std::vector<RootState> states(roots.size());
    for (size_t i = 0; i < roots.size(); ++i) states[i].directories.emplace_back(roots[i], 0);

    auto fillEntries = [&](RootState& state) {
      while (state.entries.empty() && !state.directories.empty()) {
        if (DiscoveryCanceled(stopToken, generation)) return false;
        auto [directory, depth] = std::move(state.directories.front());
        state.directories.pop_front();
        std::error_code ec;
        std::vector<std::filesystem::path> sorted;
        size_t enumerated = 0;
        for (std::filesystem::directory_iterator it(
                 directory, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
          sorted.push_back(it->path());
          if ((++enumerated & 63u) == 0 && DiscoveryCanceled(stopToken, generation)) {
            return false;
          }
        }
        if (DiscoveryCanceled(stopToken, generation)) return false;
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
          return Lower(left.wstring()) < Lower(right.wstring());
        });
        for (auto& path : sorted) state.entries.emplace_back(std::move(path), depth);
      }
      return true;
    };

    constexpr size_t kRootBatch = 64;
    bool madeProgress = true;
    while (files.size() < limit && madeProgress && !DiscoveryCanceled(stopToken, generation)) {
      madeProgress = false;
      for (auto& state : states) {
        if (!fillEntries(state)) return;
        size_t batch = 0;
        while (!state.entries.empty() && batch++ < kRootBatch && files.size() < limit) {
          if (DiscoveryCanceled(stopToken, generation)) return;
          madeProgress = true;
          auto [path, depth] = std::move(state.entries.front());
          state.entries.pop_front();
          const std::wstring name = path.filename().wstring();
          if (name.empty() || name.front() == L'.') continue;
          const DWORD attrs = GetFileAttributesW(path.c_str());
          if (attrs == INVALID_FILE_ATTRIBUTES ||
              (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT))) {
            continue;
          }
          const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
          if (isDirectory && depth < kFileIndexDepth) state.directories.emplace_back(path, depth + 1);

          const std::wstring full = path.wstring();
          if (!seen.insert(Lower(full)).second) continue;
          long long lastWriteTime = 0;
          long long fileSize = 0;
          std::error_code metaEc;
          const auto writeTime = std::filesystem::last_write_time(path, metaEc);
          if (!metaEc) lastWriteTime = writeTime.time_since_epoch().count();
          if (!isDirectory) {
            fileSize = static_cast<long long>(std::filesystem::file_size(path, metaEc));
            if (metaEc) fileSize = 0;
          }

          AppEntry app;
          app.id = L"file:" + full;
          app.name = name;
          app.path = full;
          app.source = L"file";
          app.launchType = LaunchType::Exe;
          app.launchTarget = full;
          app.iconKey = full;
          app.fileIsDirectory = isDirectory;
          app.fileLastWriteTime = lastWriteTime;
          app.fileSize = fileSize;
          app.fileIndexedAt = indexedAt;
          files.push_back(std::move(app));
        }
      }
    }
    if (DiscoveryCanceled(stopToken, generation)) return;
    std::vector<feathercast::storage::FileIndexEntry> storageEntries;
    storageEntries.reserve(files.size());
    for (const auto& file : files) storageEntries.push_back(StorageFileEntry(file));
    {
      std::lock_guard lock(dataMutex_);
      if (latestDiscoveryGeneration_.load(std::memory_order_acquire) != generation) return;
      fileIndex_ = std::move(files);
    }
    MarkSearchDataChanged();
    if (DiscoveryCanceled(stopToken, generation)) return;
    {
      std::lock_guard lock(storageMutex_);
      if (storage_.IsOpen()) storage_.UpdateFileIndex(storageEntries);
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

    currencyThread_ = std::jthread([this](std::stop_token stopToken) {
      if (stopToken.stop_requested() || stopThreads_) return;
      const auto body = HttpsGet(L"open.er-api.com", L"/v6/latest/USD");
      if (!body || stopToken.stop_requested() || stopThreads_) return;
      auto rates = RatesFromJson(*body);
      if (rates.empty()) return;
      CurrencyRates updated;
      updated.perUsd = std::move(rates);
      updated.fetchedAt = static_cast<long long>(std::time(nullptr));
      SaveCurrencyCache(updated);
      {
        std::lock_guard lock(dataMutex_);
        currencyRates_ = std::move(updated);
      }
      if (!stopThreads_) PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
    });
  }

  void CleanupFinishedUpdateThread() {
    if (updateThread_.joinable() && !updateWorkerRunning_) updateThread_.join();
  }

  void FinishUpdateTask(UpdateTaskResult result) {
    {
      std::lock_guard lock(updateMutex_);
      latestUpdateResult_ = std::move(result);
    }
    updateWorkerRunning_ = false;
    if (!stopThreads_ && hwnd_) PostMessageW(hwnd_, WM_UPDATE_READY, 0, 0);
  }

  void StartAutomaticUpdateCheck() {
    if (!settings_.updateChecksEnabled) return;
    const long long now = UnixNow();
    if (settings_.lastUpdateAttempt > 0 && now - settings_.lastUpdateAttempt < 3600) return;
    if (settings_.lastUpdateCheck > 0 && now - settings_.lastUpdateCheck < 24 * 3600) return;
    StartUpdateCheck(false);
  }

  void StartUpdateCheck(bool manual) {
    CleanupFinishedUpdateThread();
    if (updateWorkerRunning_) {
      if (manual) {
        MessageBoxW(hwnd_, L"An update check is already running.", L"FeatherCast Updates",
                    MB_OK | MB_ICONINFORMATION);
      }
      return;
    }

    settings_.lastUpdateAttempt = UnixNow();
    PersistSettings();
    const std::wstring dismissedVersion = settings_.dismissedUpdateVersion;

    updateWorkerRunning_ = true;
    AppendUpdateLog(manual ? L"Manual update check started" : L"Automatic update check started");
    updateThread_ = std::jthread([this, manual, dismissedVersion](std::stop_token stopToken) {
      UpdateTaskResult result;
      result.manual = manual;
      result.kind = UpdateTaskKind::Check;

      const auto body = HttpsGet(L"api.github.com", L"/repos/GenericLeon0/FeatherCast/releases/latest");
      if (stopToken.stop_requested() || stopThreads_) {
        updateWorkerRunning_ = false;
        return;
      }
      if (!body) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Unable to reach GitHub Releases.";
        FinishUpdateTask(std::move(result));
        return;
      }

      auto release = feathercast::updater::ParseGitHubReleaseJson(*body);
      if (!release) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"GitHub returned update metadata FeatherCast could not parse.";
        FinishUpdateTask(std::move(result));
        return;
      }

      if (!feathercast::updater::IsEligibleRelease(*release, kFeatherCastVersion)) {
        result.status = UpdateTaskStatus::NoUpdate;
        result.message = L"FeatherCast is up to date.";
        FinishUpdateTask(std::move(result));
        return;
      }

      if (!manual && !dismissedVersion.empty() && Lower(release->tagName) == Lower(dismissedVersion)) {
        result.status = UpdateTaskStatus::NoUpdate;
        result.message = L"Newest release was already dismissed: " + release->tagName;
        FinishUpdateTask(std::move(result));
        return;
      }

      const auto installer = feathercast::updater::SelectInstallerAsset(*release);
      const auto hash = installer ? feathercast::updater::SelectSha256Asset(*release, *installer) : std::nullopt;
      if (!installer || !hash) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Latest release is missing the required installer or SHA-256 asset.";
        result.release = std::move(*release);
        FinishUpdateTask(std::move(result));
        return;
      }

      result.status = UpdateTaskStatus::Available;
      result.message = L"Update available: " + release->tagName;
      result.release = std::move(*release);
      FinishUpdateTask(std::move(result));
    });
  }

  void StartUpdateDownload(feathercast::updater::ReleaseInfo release, bool manual) {
    CleanupFinishedUpdateThread();
    if (updateWorkerRunning_) {
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

    updateWorkerRunning_ = true;
    AppendUpdateLog(L"Update download started: " + release.tagName);
    updateThread_ = std::jthread([this, release = std::move(release), manual](std::stop_token stopToken) mutable {
      UpdateTaskResult result;
      result.manual = manual;
      result.kind = UpdateTaskKind::DownloadInstall;
      result.release = release;

      const auto installer = feathercast::updater::SelectInstallerAsset(release);
      const auto hash = installer ? feathercast::updater::SelectSha256Asset(release, *installer) : std::nullopt;
      if (!installer || !hash) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Update release is missing the required installer or SHA-256 asset.";
        FinishUpdateTask(std::move(result));
        return;
      }

      const auto hashText = HttpsGetUrl(hash->browserDownloadUrl, 64 * 1024);
      if (stopToken.stop_requested() || stopThreads_) {
        updateWorkerRunning_ = false;
        return;
      }
      if (!hashText || !feathercast::updater::ExtractSha256Hex(*hashText)) {
        result.status = UpdateTaskStatus::Error;
        result.message = L"Unable to download or parse the update SHA-256 file.";
        FinishUpdateTask(std::move(result));
        return;
      }

      std::wstring fileName = std::filesystem::path(installer->name).filename().wstring();
      if (fileName.empty()) fileName = feathercast::updater::ExpectedInstallerAssetName(release.tagName);
      const auto installerPath = UpdatesPath() / fileName;
      if (!HttpsDownloadToFile(installer->browserDownloadUrl, installerPath, stopToken)) {
        if (stopToken.stop_requested() || stopThreads_) {
          updateWorkerRunning_ = false;
          return;
        }
        result.status = UpdateTaskStatus::Error;
        result.message = L"Unable to download the update installer.";
        FinishUpdateTask(std::move(result));
        return;
      }

      if (!feathercast::updater::VerifyFileSha256(installerPath, *hashText)) {
        std::error_code ec;
        std::filesystem::remove(installerPath, ec);
        result.status = UpdateTaskStatus::Error;
        result.message = L"Downloaded installer failed SHA-256 verification.";
        FinishUpdateTask(std::move(result));
        return;
      }
      if (!feathercast::updater::VerifyAuthenticodeSigner(
              installerPath, kFeatherCastExpectedPublisher,
              kFeatherCastAllowedSignerThumbprints)) {
        std::error_code ec;
        std::filesystem::remove(installerPath, ec);
        result.status = UpdateTaskStatus::Error;
        result.message = L"Downloaded installer has an invalid or unexpected Authenticode signature.";
        FinishUpdateTask(std::move(result));
        return;
      }

      result.status = UpdateTaskStatus::ReadyToInstall;
      result.installerPath = installerPath;
      result.message = L"Verified update installer: " + installerPath.wstring();
      FinishUpdateTask(std::move(result));
    });
  }

  void OnUpdateReady() {
    CleanupFinishedUpdateThread();

    std::optional<UpdateTaskResult> result;
    {
      std::lock_guard lock(updateMutex_);
      if (latestUpdateResult_) result = std::move(latestUpdateResult_);
      latestUpdateResult_.reset();
    }
    if (!result) return;

    AppendUpdateLog(result->message);
    if (result->kind == UpdateTaskKind::Check && result->status != UpdateTaskStatus::Error) {
      settings_.lastUpdateCheck = UnixNow();
      PersistSettings();
    }

    if (result->kind == UpdateTaskKind::Check) {
      if (result->status == UpdateTaskStatus::NoUpdate) {
        if (result->manual) {
          const std::wstring message = L"FeatherCast " + std::wstring(kFeatherCastVersion) + L" is up to date.";
          MessageBoxW(hwnd_, message.c_str(), L"FeatherCast Updates", MB_OK | MB_ICONINFORMATION);
        }
        return;
      }
      if (result->status == UpdateTaskStatus::Error) {
        if (result->manual) {
          MessageBoxW(hwnd_, result->message.c_str(), L"FeatherCast Updates", MB_OK | MB_ICONWARNING);
        }
        return;
      }
      if (result->status == UpdateTaskStatus::Available) {
        const std::wstring releaseVersion = feathercast::updater::AssetVersionFromTag(result->release.tagName);
        if (!result->manual) {
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
          StartUpdateDownload(std::move(result->release), result->manual);
        } else if (!result->manual) {
          settings_.dismissedUpdateVersion = result->release.tagName;
          PersistSettings();
        }
        return;
      }
    }

    if (result->kind == UpdateTaskKind::DownloadInstall) {
      if (result->status == UpdateTaskStatus::ReadyToInstall) {
        const std::wstring releaseVersion = feathercast::updater::AssetVersionFromTag(result->release.tagName);
        const std::wstring message =
            L"The FeatherCast " + releaseVersion +
            L" installer was downloaded and verified.\n\nInstall it now? FeatherCast will close.";
        const int choice = MessageBoxW(hwnd_, message.c_str(), L"FeatherCast Updates",
                                       MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (choice != IDYES) return;

        HINSTANCE launched = ShellExecuteW(nullptr, L"open", result->installerPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32) {
          MessageBoxW(hwnd_, L"FeatherCast could not start the verified installer.", L"FeatherCast Updates",
                      MB_OK | MB_ICONWARNING);
          return;
        }
        DestroyWindow(hwnd_);
        return;
      }

      MessageBoxW(hwnd_, result->message.c_str(), L"FeatherCast Updates", MB_OK | MB_ICONWARNING);
    }
  }

  std::optional<std::vector<AppEntry>> DiscoverApps(std::stop_token stopToken,
                                                    uint64_t generation) {
    std::vector<AppEntry> apps;
    std::map<std::wstring, size_t> identities;

    auto addUnique = [&](AppEntry entry) {
      if (entry.id.empty() || entry.name.empty()) return;
      std::wstring identity;
      if (!entry.appUserModelId.empty()) identity = L"aumid:" + Lower(entry.appUserModelId);
      else if (!entry.targetPath.empty()) identity = L"target:" + Lower(entry.targetPath);
      else if (!entry.launchTarget.empty()) identity = L"launch:" + Lower(entry.launchTarget);
      else identity = L"id:" + Lower(entry.id);

      if (const auto found = identities.find(identity); found != identities.end()) {
        auto& existing = apps[found->second];
        existing.adminSupported = existing.adminSupported || entry.adminSupported;
        existing.systemEssential = existing.systemEssential || entry.systemEssential;
        if (existing.iconKey.empty()) existing.iconKey = std::move(entry.iconKey);
        if (existing.targetPath.empty()) existing.targetPath = std::move(entry.targetPath);
        existing.keywords = UniqueKeywords({
            feathercast::core::JoinKeywords(existing.keywords),
            feathercast::core::JoinKeywords(entry.keywords),
        });
        return;
      }
      identities.emplace(std::move(identity), apps.size());
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
    return {
      CommandDisplay(CommandKind::ClipboardHistory, L"Clipboard History", L"Browse and paste copied items", {L"clipboard", L"history", L"paste", L"copy"}),
      CommandDisplay(CommandKind::EmojiPicker, L"Search Emoji", L"Browse and paste emoji", {L"emoji", L"emoticon", L"smiley", L"symbol", L"face"}),
      CommandDisplay(CommandKind::Settings, L"Settings", L"Open FeatherCast settings", {L"preferences", L"options", L"shortcut"}),
      CommandDisplay(CommandKind::Quit, L"Quit FeatherCast", L"Exit the background launcher", {L"exit", L"close"}),
      CommandDisplay(CommandKind::Restart, L"Restart FeatherCast", L"Restart the native app", {L"reload"}),
      CommandDisplay(CommandKind::RefreshApps, L"Refresh App Index", L"Rescan Start Menu and Store apps", {L"rescan", L"reload apps"}),
      CommandDisplay(CommandKind::ClearIconCache, L"Clear Icon Cache", L"Delete cached shell icons", {L"icons", L"cache"}),
      CommandDisplay(CommandKind::ClearRecents, L"Clear Recents", L"Forget recently used apps", {L"history", L"recent apps"}),
      CommandDisplay(CommandKind::OpenDataFolder, L"Open Settings Folder", L"Open roaming FeatherCast configuration", {L"settings json", L"snippets", L"theme", L"plugins"}),
      CommandDisplay(CommandKind::OpenLocalDataFolder, L"Open Local Data Folder", L"Open logs, cache, database, and updates", {L"logs", L"cache", L"database", L"updates"}),
      CommandDisplay(CommandKind::ReloadExtensions, L"Reload Extensions", L"Reload plugin manifests and helper processes", {L"plugins", L"extensions", L"dll"}),
      CommandDisplay(CommandKind::CheckForUpdates, L"Check for Updates", L"Find and install the latest FeatherCast release", {L"update", L"upgrade", L"release"}),
      CommandDisplay(CommandKind::ClearClipboardHistory, L"Clear Clipboard History", L"Forget saved clipboard entries", {L"clipboard", L"history", L"clear"}),
      CommandDisplay(CommandKind::OpenSnippetsFile, L"Open Snippets File", L"Edit reusable text snippets", {L"snippet", L"snippets json", L"text expansion"}),
      CommandDisplay(CommandKind::ReloadSnippets, L"Reload Snippets", L"Reload snippets.json from disk", {L"snippet", L"reload", L"text expansion"}),
      CommandDisplay(CommandKind::OpenThemeFile, L"Open Theme File", L"Edit theme.json styling", {L"theme", L"appearance", L"json", L"style"}),
      CommandDisplay(CommandKind::ReloadTheme, L"Reload Theme", L"Reload theme.json styling", {L"theme", L"appearance", L"reload", L"style"}),
      CommandDisplay(CommandKind::LockPC, L"Lock PC", L"Lock this computer", {L"lock", L"workstation", L"secure"}),
      CommandDisplay(CommandKind::SleepPC, L"Sleep PC", L"Put this computer to sleep", {L"sleep", L"suspend", L"standby"}),
      CommandDisplay(CommandKind::MuteAudio, L"Mute Audio", L"Toggle system audio mute", {L"mute", L"sound", L"volume", L"audio"}),
      CommandDisplay(CommandKind::ShutDown, L"Shut Down PC", L"Power off this computer", {L"shutdown", L"power off", L"turn off"}),
      CommandDisplay(CommandKind::RestartPC, L"Restart PC", L"Reboot this computer", {L"reboot", L"restart computer"}),
      CommandDisplay(CommandKind::EmptyRecycleBin, L"Empty Recycle Bin", L"Permanently delete recycle bin contents", {L"trash", L"empty bin", L"recycle"}),
    };
  }

  std::vector<DisplayItem> ActionsFor(const DisplayItem& target) const {
    std::vector<DisplayItem> actions;
    if (target.isCommand || target.isAction || target.isExtension || target.isSnippet || target.isClipboard) return actions;

    if (target.isWindow) {
      actions.push_back(ActionDisplay(ActionKind::Switch, L"Switch to Window", L"Focus " + target.window.name, target));
      actions.push_back(ActionDisplay(ActionKind::Minimize, L"Minimize Window", L"Minimize " + target.window.name, target));
      actions.push_back(ActionDisplay(ActionKind::MaximizeRestore, L"Maximize or Restore Window", L"Toggle window state", target));
      actions.push_back(ActionDisplay(ActionKind::CloseWindow, L"Close Window", L"Send close request", target));
      return actions;
    }

    actions.push_back(ActionDisplay(ActionKind::Open, L"Open", L"Launch " + target.app.name, target));
    if (target.app.adminSupported) {
      actions.push_back(ActionDisplay(ActionKind::RunAsAdmin, L"Run as Administrator", L"Launch elevated", target));
    }
    if (target.app.launchType != LaunchType::Shell && (!target.app.path.empty() || !target.app.targetPath.empty())) {
      actions.push_back(ActionDisplay(ActionKind::OpenLocation, L"Open File Location", L"Show app shortcut or target", target));
    }
    if (!target.app.path.empty() || !target.app.targetPath.empty() || !target.app.launchTarget.empty()) {
      actions.push_back(ActionDisplay(ActionKind::CopyPath, L"Copy Path", L"Copy app shortcut or target path", target));
    }
    if (ContainsAnyAppKey(settings_.pinnedApps, target.app)) {
      actions.push_back(ActionDisplay(ActionKind::Unpin, L"Unpin", L"Remove from pinned apps", target));
    } else {
      actions.push_back(ActionDisplay(ActionKind::Pin, L"Pin", L"Keep near the top of results", target));
    }
    if (ContainsAnyAppKey(settings_.hiddenApps, target.app)) {
      actions.push_back(ActionDisplay(ActionKind::Unhide, L"Unhide", L"Show in launcher results", target));
    } else {
      actions.push_back(ActionDisplay(ActionKind::Hide, L"Hide", L"Remove from launcher results", target));
    }
    return actions;
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
    bool schedule = false;
    {
      std::lock_guard lock(settingsPersistenceMutex_);
      pendingSettingsSave_ = settings_;
      if (!settingsSaveScheduled_) {
        settingsSaveScheduled_ = true;
        schedule = true;
      }
    }
    if (schedule && !persistenceExecutor_.Submit([this](std::stop_token) {
          for (;;) {
            std::optional<Settings> pending;
            {
              std::lock_guard lock(settingsPersistenceMutex_);
              if (!pendingSettingsSave_) {
                settingsSaveScheduled_ = false;
                return;
              }
              pending = std::move(pendingSettingsSave_);
              pendingSettingsSave_.reset();
            }
            if (!SaveSettings(*pending) && !stopThreads_ &&
                !persistenceErrorQueued_.exchange(true, std::memory_order_acq_rel)) {
              PostMessageW(hwnd_, WM_PERSISTENCE_ERROR, 0, 0);
            }
          }
        })) {
      {
        std::lock_guard lock(settingsPersistenceMutex_);
        settingsSaveScheduled_ = false;
        pendingSettingsSave_.reset();
      }
      const bool saved = SaveSettings(settings_);
      if (!saved && !persistenceErrorQueued_.exchange(true, std::memory_order_acq_rel)) {
        PostMessageW(hwnd_, WM_PERSISTENCE_ERROR, 0, 0);
      }
    }
    MarkSearchDataChanged();
    return true;
  }

  void RequestSearch() {
    QueryRequest req = GatherRequest();
    if (visible_) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd_, OBJID_CLIENT, 1);
    if (!req.compactClear && !req.empty && !req.actionMode) {
      extensions_.RequestQuery(req.query, req.generation);
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
    req.generation = ++searchGeneration_;
    latestRequestedSearchGeneration_.store(req.generation, std::memory_order_release);
    req.latestGeneration = &latestRequestedSearchGeneration_;
    req.query = query_;

    const bool empty = Trim(query_).empty();
    req.empty = empty;
    req.actionMode = actionMode_;
    req.browseView = browseView_;
    req.compactClear = settings_.compactMode && empty && view_ == View::Search &&
                       !actionMode_ && browseView_ == BrowseView::None;
    req.recentIds = std::set<std::wstring>(settings_.recentApps.begin(), settings_.recentApps.end());
    req.limit = std::clamp(settings_.maxResults, MIN_RESULTS, MAX_RESULT_SETTING);
    req.now = UnixNow();
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
      for (const auto& extensionResult : extensions_.CachedResultsFor(query_)) {
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
        if (item.app.systemEssential || item.app.source == L"alias" || item.app.source == L"quicklink") snap->system.push_back(item);
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
    {
      std::lock_guard lock(snapshotMutex_);
      pendingSnapshotBuild_ = std::move(request);
    }
    snapshotScheduledRevision_ = revision;
    snapshotCv_.notify_one();
  }

  void StartSnapshotWorker() {
    snapshotThread_ = std::jthread([this](std::stop_token stopToken) {
      for (;;) {
        SnapshotBuildRequest request;
        {
          std::unique_lock lock(snapshotMutex_);
          snapshotCv_.wait(lock, [&] {
            return pendingSnapshotBuild_.has_value() || stopToken.stop_requested() || stopThreads_;
          });
          if (stopToken.stop_requested() || stopThreads_) return;
          request = std::move(*pendingSnapshotBuild_);
          pendingSnapshotBuild_.reset();
        }

        auto built = BuildSnapshot(request.settings);
        if (stopToken.stop_requested() || stopThreads_) return;
        if (dataRevision_.load(std::memory_order_acquire) != request.revision) continue;
        {
          std::lock_guard lock(snapshotMutex_);
          latestSnapshotBuild_ = SnapshotBuildResult{request.revision, std::move(built)};
        }
        if (!stopThreads_) PostMessageW(hwnd_, WM_SNAPSHOT_READY, 0, 0);
      }
    });
  }

  void OnSnapshotReady() {
    std::optional<SnapshotBuildResult> result;
    {
      std::lock_guard lock(snapshotMutex_);
      if (latestSnapshotBuild_) result = std::move(latestSnapshotBuild_);
      latestSnapshotBuild_.reset();
    }
    if (!result || !result->snapshot) return;

    const uint64_t currentRevision = dataRevision_.load(std::memory_order_acquire);
    if (result->revision != currentRevision) {
      ScheduleSnapshotBuild(currentRevision);
      return;
    }
    snapshot_ = std::move(result->snapshot);
    snapshotRevision_ = result->revision;
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // Pure engine: QueryRequest -> ResultsCollection. No member/UI state access,
  // so it is safe to run on the search worker thread.
  static ResultsCollection ComputeResults(const QueryRequest& req) {
    ResultsCollection result;
    result.generation = req.generation;

    std::vector<Section> sections;
    std::set<std::wstring> used;
    auto take = [&](const std::vector<DisplayItem>& items, size_t limit = SIZE_MAX) {
      std::vector<DisplayItem> out;
      for (const auto& item : items) {
        const auto key = item.Key();
        if (key.empty() || used.contains(key)) continue;
        used.insert(key);
        out.push_back(item);
        if (out.size() >= limit) break;
      }
      return out;
    };
    auto addSection = [&](std::wstring title, std::vector<DisplayItem> items) {
      if (!items.empty()) sections.push_back({std::move(title), std::move(items)});
    };

    if (req.compactClear) {
      // Render nothing.
    } else if (req.browseView == BrowseView::Clipboard) {
      if (req.empty) {
        addSection(L"Clipboard History", take(req.snapshot->clipboardItems));
      } else {
        auto order = feathercast::core::Search(req.query, req.snapshot->clipboardSearchItems);
        std::vector<DisplayItem> hits;
        for (const auto index : order) hits.push_back(req.snapshot->clipboardItems[index]);
        addSection(L"Clipboard History", take(hits));
      }
    } else if (req.browseView == BrowseView::Emoji) {
      std::vector<DisplayItem> emojiItems;
      for (const auto& emoji : feathercast::emoji::SearchEmoji(req.query, 300)) {
        emojiItems.push_back(SymbolDisplay(emoji));
      }
      addSection(L"Emoji", take(emojiItems));
    } else if (req.actionMode) {
      if (req.empty) {
        addSection(L"Actions", take(req.actions));
      } else {
        auto order = feathercast::core::Search(req.query, req.actionSearchItems);
        std::vector<DisplayItem> hits;
        for (const auto index : order) hits.push_back(req.actions[index]);
        addSection(L"Actions", take(hits));
      }
    } else if (req.empty) {
      addSection(L"Pinned", take(req.snapshot->pinned, 12));
      addSection(L"Recently used", take(req.snapshot->recent, 8));
      addSection(L"Open windows", take(req.snapshot->windowItems));
      addSection(L"Snippets", take(req.snapshot->snippetItems, 8));
      addSection(L"Clipboard History", take(req.snapshot->clipboardItems, 5));
      addSection(L"System Folders", take(req.snapshot->systemFolders, 12));
      addSection(L"System essentials", take(req.snapshot->system, 8));
      addSection(L"Commands", take(req.snapshot->commandItems, 8));
    } else {
      const std::wstring trimmed = Trim(req.query);
      if (StartsWith(trimmed, L">")) {
        if (const auto command = feathercast::run_command::Classify(trimmed)) {
          addSection(command->kind == feathercast::run_command::Kind::OpenTarget ? L"Open" : L"Run",
                     take({RunCommandDisplay(*command)}, 1));
        }
      } else if (StartsWith(trimmed, L":")) {
        std::vector<DisplayItem> symbolItems;
        for (const auto& symbol : feathercast::symbols::SearchSymbols(trimmed, 40)) {
          symbolItems.push_back(SymbolDisplay(symbol));
        }
        addSection(L"Symbols", take(symbolItems, 40));
      } else {
      // Web search: "<keyword> <terms>" where the keyword is a configured engine.
      if (!trimmed.empty()) {
        const size_t space = trimmed.find_first_of(L" \t");
        if (space != std::wstring::npos) {
          const std::wstring keyword = Lower(trimmed.substr(0, space));
          const std::wstring terms = Trim(trimmed.substr(space + 1));
          if (!terms.empty()) {
            if (auto engine = req.searchEngines.find(keyword); engine != req.searchEngines.end()) {
              addSection(L"Web Search", take({WebSearchDisplay(keyword, engine->second, terms)}, 1));
            }
          }
        }
      }

      if (const auto calculation = feathercast::calculator::TryEvaluate(req.query)) {
        addSection(L"Calculator", take({CalculatorDisplay(*calculation)}, 1));
      }

      if (const auto conversion = feathercast::converter::TryConvert(req.query, req.currencyRates, req.defaultCurrency)) {
        addSection(L"Conversion", take({ConversionDisplay(conversion->expression, conversion->display)}, 1));
      }

      addSection(L"Extensions", take(req.extensionItems, 20));

      feathercast::core::SearchOptions searchOptions;
      searchOptions.limit = static_cast<size_t>(req.limit);
      searchOptions.now = req.now;
      searchOptions.generation = req.generation;
      searchOptions.latestGeneration = req.latestGeneration;
      auto order = feathercast::core::SearchPrepared(
          req.query, req.snapshot->searchItems, req.recentIds, searchOptions);

      std::vector<DisplayItem> hits;
      hits.reserve(order.size());
      for (const auto index : order) hits.push_back(req.snapshot->pool[index]);

      if (!hits.empty()) addSection(L"Best match", take({hits.front()}, 1));

      std::vector<DisplayItem> rest(hits.size() > 1 ? hits.begin() + 1 : hits.end(), hits.end());
      std::vector<DisplayItem> recent;
      std::vector<DisplayItem> appsOnly;
      std::vector<DisplayItem> open;
      std::vector<DisplayItem> system;
      std::vector<DisplayItem> commands;
      std::vector<DisplayItem> quicklinks;
      std::vector<DisplayItem> snippets;
      std::vector<DisplayItem> clipboard;
      std::vector<DisplayItem> files;
      std::vector<DisplayItem> systemFolders;
      std::vector<DisplayItem> other;
      for (const auto& item : rest) {
        const bool plainApp = !item.isWindow && !item.isCommand && !item.isSnippet && !item.isClipboard;
        if (item.isSnippet) snippets.push_back(item);
        if (item.isClipboard) clipboard.push_back(item);
        if (item.isCommand) commands.push_back(item);
        if (plainApp && item.app.source == L"quicklink") quicklinks.push_back(item);
        if (plainApp && item.app.source == L"file") files.push_back(item);
        if (plainApp && item.app.source == L"system-folder") systemFolders.push_back(item);
        if (plainApp && item.app.source != L"file" && req.recentIds.contains(PrimaryAppId(item.app))) recent.push_back(item);
        if (plainApp && item.app.source == L"shortcut") appsOnly.push_back(item);
        if (item.isWindow) open.push_back(item);
        if (plainApp && item.app.source != L"shortcut" && item.app.source != L"quicklink" &&
            item.app.source != L"file" && item.app.source != L"system-folder") {
          system.push_back(item);
        }
        other.push_back(item);
      }
      addSection(L"Commands", take(commands, 20));
      addSection(L"Quicklinks", take(quicklinks, 20));
      addSection(L"Snippets", take(snippets, 20));
      addSection(L"Clipboard History", take(clipboard, 20));
      addSection(L"Recently used", take(recent, 8));
      addSection(L"Apps", take(appsOnly, 80));
      addSection(L"Files & Folders", take(files, 40));
      addSection(L"System Folders", take(systemFolders, 30));
      addSection(L"Open windows", take(open, 40));
      addSection(L"System & Store apps", take(system, 80));
      addSection(L"Other matches", take(other, 40));
      }
    }

    for (const auto& section : sections) {
      result.flatItems.insert(result.flatItems.end(), section.items.begin(), section.items.end());
    }
    result.sections = std::move(sections);
    return result;
  }

  // UI thread: commit a freshly computed result set to the rendered state.
  void ApplyResults(ResultsCollection result) {
    sections_ = std::move(result.sections);
    flatItems_ = std::move(result.flatItems);
    if (pendingNavigationRestore_) {
      const auto& restore = *pendingNavigationRestore_;
      const auto match = std::find_if(
          flatItems_.begin(), flatItems_.end(),
          [&](const DisplayItem& item) { return item.Key() == restore.selectedKey; });
      selected_ = match != flatItems_.end()
                      ? static_cast<int>(std::distance(flatItems_.begin(), match))
                      : restore.selected;
      scroll_ = restore.scroll;
      pendingNavigationRestore_.reset();
    }
    if (selected_ >= static_cast<int>(flatItems_.size())) selected_ = std::max<int>(0, static_cast<int>(flatItems_.size()) - 1);
    ApplyWindowSize();
    SyncSelectionAnimationToTarget();
    NotifyWinEvent(EVENT_OBJECT_REORDER, hwnd_, OBJID_CLIENT, CHILDID_SELF);
  }

  // Hand the newest request to the worker, coalescing any unstarted request.
  void DispatchToWorker(QueryRequest req) {
    {
      std::lock_guard lock(searchMutex_);
      pendingRequest_ = std::move(req);
    }
    searchCv_.notify_one();
  }

  void StartSearchWorker() {
    searchThread_ = std::jthread([this](std::stop_token stopToken) {
      for (;;) {
        QueryRequest req;
        {
          std::unique_lock lock(searchMutex_);
          searchCv_.wait(lock, [&] {
            return pendingRequest_.has_value() || stopToken.stop_requested() || stopThreads_;
          });
          if (stopToken.stop_requested() || stopThreads_) return;
          req = std::move(*pendingRequest_);
          pendingRequest_.reset();
        }
        ResultsCollection result = ComputeResults(req);
        {
          std::lock_guard lock(searchMutex_);
          latestResult_ = std::move(result);
        }
        if (!stopThreads_) PostMessageW(hwnd_, WM_SEARCH_READY, 0, 0);
      }
    });
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
    if (lastActiveWindow_ && IsWindow(lastActiveWindow_)) return MonitorFromWindow(lastActiveWindow_, MONITOR_DEFAULTTONEAREST);
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
    bool isForeground = visible_ || (settingsHwnd_ && IsWindowVisible(settingsHwnd_));
    if (isForeground) {
      KillTimer(hwnd_, TIMER_MEM_TRIM);
      SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

      MEMORY_PRIORITY_INFORMATION memPriority{};
      memPriority.MemoryPriority = MEMORY_PRIORITY_NORMAL;
      SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memPriority, sizeof(memPriority));

      PROCESS_POWER_THROTTLING_STATE powerThrottling{};
      powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
      powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      powerThrottling.StateMask = 0;
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &powerThrottling, sizeof(powerThrottling));
    } else {
      // Retain prepared search, rendering, icon, emoji, and extension state so
      // the next launcher invocation stays warm. Only query-specific results
      // are discarded; reusable capacity is intentionally kept.
      {
        std::lock_guard lock(searchMutex_);
        latestResult_ = ResultsCollection{};
        pendingRequest_.reset();
      }
      ++searchGeneration_;
      latestRequestedSearchGeneration_.store(searchGeneration_, std::memory_order_release);
      sections_.clear();
      flatItems_.clear();
      hits_.clear();

      // Background work remains unobtrusive without forcing page faults,
      // re-decoding icons, or restarting plugin hosts on every reopen.
      SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

      MEMORY_PRIORITY_INFORMATION memPriority{};
      memPriority.MemoryPriority = MEMORY_PRIORITY_LOW;
      SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memPriority, sizeof(memPriority));

      PROCESS_POWER_THROTTLING_STATE powerThrottling{};
      powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
      powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      powerThrottling.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &powerThrottling, sizeof(powerThrottling));
    }
  }

  void ShowOverlay(View view) {
    view_ = view;
    HWND foreground = GetForegroundWindow();
    if (!visible_) {
      if (foreground && foreground != hwnd_) lastActiveWindow_ = foreground;
    }
    overlayMonitor_ = ResolveOverlayMonitor(foreground);
    visible_ = true;
    UpdateBackgroundState();
    navigationStack_.clear();
    pendingNavigationRestore_.reset();
    actionMode_ = false;
    browseView_ = BrowseView::None;
    ClearQuery();
    selected_ = 0;
    scroll_ = 0;
    SyncSelectionAnimationToTarget();
    RequestSearch();
    PositionWindow();
    // Reassert the transparent window treatment each reveal.
    ApplyGlass(hwnd_);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    RefreshWindowsAsync();

    // Auto-focus overlay
    SendVirtualKeyTap(0xE8);

    DWORD foregroundThreadId = GetWindowThreadProcessId(foreground, nullptr);
    DWORD currentThreadId = GetCurrentThreadId();
    if (foreground && foreground != hwnd_ && foregroundThreadId != currentThreadId) {
      AttachThreadInput(foregroundThreadId, currentThreadId, TRUE);
      SetForegroundWindow(hwnd_);
      SetActiveWindow(hwnd_);
      SetFocus(hwnd_);
      AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
    } else {
      SetForegroundWindow(hwnd_);
      SetActiveWindow(hwnd_);
      SetFocus(hwnd_);
    }

    // Ignore the mouse pointer until it is actually moved, so an overlay that
    // opens under the cursor does not hover-select the result beneath it.
    GetCursorPos(&mouseAnchor_);
    ignoreMouseUntilMove_ = true;

    SetTimer(hwnd_, 1, 200, nullptr);
    animating_ = AnimationsAllowed();
    if (animating_) {
      animStartQpc_ = NowQpc();
      lastAnimationFrameQpc_ = animStartQpc_;
      RequestAnimationFrame();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void HideOverlay(bool restoreFocus) {
    KillTimer(hwnd_, 1);
    animating_ = false;
    animatingSelection_ = false;
    animationFrameQueued_ = false;
    visualSelectedY_ = -1.0f;
    StopSelectionAnimationTimer();
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    overlayMonitor_ = nullptr;
    UpdateBackgroundState();
    if (restoreFocus && lastActiveWindow_) {
      FocusWindow(lastActiveWindow_);
      lastActiveWindow_ = nullptr;
    }
  }

  void ToggleOverlay() {
    if (visible_) HideOverlay(true);
    else ShowOverlay(View::Search);
  }

  void TriggerShortcutToggle() {
    const ULONGLONG now = GetTickCount64();
    if (now - lastShortcutToggleTick_ < 250) return;
    lastShortcutToggleTick_ = now;
    ToggleOverlay();
  }

  void OpenSettings() {
    HideOverlay(false);
    recording_ = false;
    shortcutRecorder_.Reset();
    pendingShortcut_.clear();
    settingsHover_ = -1;
    settingsFocusIndex_ = 0;
    ShowSettingsWindow();
  }

  void ShowSettingsWindow() {
    if (!settingsHwnd_) return;
    settingsScroll_ = 0.0f;
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    const float scale = GetMonitorScale(monitor);

    const int width = static_cast<int>(SETTINGS_WIDTH * scale);
    const int height = static_cast<int>(SettingsContentHeight() * scale);

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const int maxHeight = mi.rcWork.bottom - mi.rcWork.top - static_cast<int>(40 * scale);
    const int clamped = std::min(height, maxHeight);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
    const int yPos = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - clamped) / 2;

    SetWindowPos(settingsHwnd_, HWND_NOTOPMOST, x, yPos, width, clamped, SWP_NOACTIVATE);
    // Reassert the transparent window treatment now that the window is sized.
    ApplyGlass(settingsHwnd_);
    ShowWindow(settingsHwnd_, SW_SHOW);
    SetForegroundWindow(settingsHwnd_);
    SetActiveWindow(settingsHwnd_);
    SetFocus(settingsHwnd_);
    InvalidateRect(settingsHwnd_, nullptr, FALSE);
    UpdateBackgroundState();
  }

  void HideSettings() {
    recording_ = false;
    shortcutRecorder_.Reset();
    if (settingsHwnd_) ShowWindow(settingsHwnd_, SW_HIDE);
    UpdateBackgroundState();
  }

  void EnterActionMode(const DisplayItem& target) {
    if (target.isCommand || target.isAction || target.isExtension || target.isSnippet ||
        target.isClipboard || target.isRunCommand || target.isSymbol) return;
    PushNavigationState();
    actionMode_ = true;
    actionTarget_ = target;
    ClearQuery();
    selected_ = 0;
    scroll_ = 0;
    SyncSelectionAnimationToTarget();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ExitActionMode() {
    RestoreNavigationState();
  }

  // Open a dedicated clipboard/emoji browse view in place of the result list.
  void EnterBrowseView(BrowseView view) {
    PushNavigationState();
    actionMode_ = false;
    browseView_ = view;
    ClearQuery();
    selected_ = 0;
    scroll_ = 0;
    SyncSelectionAnimationToTarget();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ExitBrowseView() {
    RestoreNavigationState();
  }

  void PushNavigationState() {
    NavigationState state;
    state.view = view_;
    state.actionMode = actionMode_;
    state.browseView = browseView_;
    state.query = query_;
    state.caret = caret_;
    state.selectionAnchor = selectionAnchor_;
    state.selected = selected_;
    state.scroll = scroll_;
    if (selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) {
      state.selectedKey = flatItems_[selected_].Key();
    }
    navigationStack_.push_back(std::move(state));
  }

  void RestoreNavigationState() {
    if (navigationStack_.empty()) {
      actionMode_ = false;
      browseView_ = BrowseView::None;
      ClearQuery();
      selected_ = 0;
      scroll_ = 0;
    } else {
      NavigationState state = std::move(navigationStack_.back());
      navigationStack_.pop_back();
      view_ = state.view;
      actionMode_ = state.actionMode;
      browseView_ = state.browseView;
      query_ = state.query;
      imeComposition_.clear();
      caret_ = std::min(state.caret, query_.size());
      selectionAnchor_ = state.selectionAnchor;
      if (selectionAnchor_) *selectionAnchor_ = std::min(*selectionAnchor_, query_.size());
      selected_ = state.selected;
      scroll_ = state.scroll;
      pendingNavigationRestore_ = std::move(state);
    }
    SyncSelectionAnimationToTarget();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // Placeholder shown in the search box when the query is empty.
  std::wstring SearchPlaceholder() const {
    if (browseView_ == BrowseView::Clipboard) return L"Search clipboard history...";
    if (browseView_ == BrowseView::Emoji) return L"Search emoji...";
    if (actionMode_) return L"Actions for " + actionTarget_.Name();
    return L"Search apps, windows, or commands...";
  }

  void ClearQuery() {
    query_.clear();
    caret_ = 0;
    selectionAnchor_.reset();
  }

  void SetQueryText(std::wstring value) {
    navigationStack_.clear();
    pendingNavigationRestore_.reset();
    query_ = std::move(value);
    caret_ = query_.size();
    selectionAnchor_.reset();
    actionMode_ = false;
    browseView_ = BrowseView::None;
    selected_ = 0;
    scroll_ = 0;
    SyncSelectionAnimationToTarget();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ClampCaret() {
    caret_ = std::min(caret_, query_.size());
    if (selectionAnchor_) *selectionAnchor_ = std::min(*selectionAnchor_, query_.size());
  }

  std::optional<std::pair<size_t, size_t>> SelectionRange() const {
    if (!selectionAnchor_ || *selectionAnchor_ == caret_) return std::nullopt;
    return std::minmax(*selectionAnchor_, caret_);
  }

  void MoveCaret(size_t next, bool extendSelection) {
    next = std::min(next, query_.size());
    if (extendSelection) {
      if (!selectionAnchor_) selectionAnchor_ = caret_;
    } else {
      selectionAnchor_.reset();
    }
    caret_ = next;
  }

  bool DeleteSelection() {
    const auto range = SelectionRange();
    if (!range) return false;
    query_.erase(range->first, range->second - range->first);
    caret_ = range->first;
    selectionAnchor_.reset();
    return true;
  }

  void CopySelectionToClipboard() {
    if (const auto range = SelectionRange()) {
      CopyTextToClipboard(query_.substr(range->first, range->second - range->first));
    }
  }

  void InsertQueryText(const std::wstring& text) {
    DeleteSelection();
    constexpr size_t kMaxQueryChars = 4096;
    const size_t room = query_.size() < kMaxQueryChars ? kMaxQueryChars - query_.size() : 0;
    const std::wstring clipped = text.substr(0, room);
    query_.insert(caret_, clipped);
    caret_ += clipped.size();
    selectionAnchor_.reset();
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
      SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top, width, height, SWP_NOACTIVATE | SWP_NOMOVE);
      ApplyRoundedRegion(width, height);
    }
  }

  LONGLONG NowQpc() const {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
  }

  double QpcElapsedMs(LONGLONG startQpc) const {
    if (!qpcFrequency_.QuadPart || !startQpc) return 0.0;
    return static_cast<double>(NowQpc() - startQpc) * 1000.0 / static_cast<double>(qpcFrequency_.QuadPart);
  }

  enum class SelectionMotion {
    Keyboard,
    Hover,
  };

  static constexpr double kSelectionHoverSettleSeconds = 0.055;
  static constexpr double kSelectionKeyboardSettleSeconds = 0.090;

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

  double ConsumeSelectionDeltaSeconds() {
    if (!qpcFrequency_.QuadPart) return 0.0;
    const LONGLONG now = NowQpc();
    if (!lastSelectionFrameQpc_) {
      lastSelectionFrameQpc_ = now;
      return 0.0;
    }
    const double dt = static_cast<double>(now - lastSelectionFrameQpc_) / static_cast<double>(qpcFrequency_.QuadPart);
    lastSelectionFrameQpc_ = now;
    return std::clamp(dt, 0.0, 0.05);
  }

  std::optional<float> SelectedRowTop() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(flatItems_.size())) return std::nullopt;
    float y = kResultsTop - static_cast<float>(scroll_);
    int row = 0;
    for (const auto& section : sections_) {
      y += kSectionHeaderHeight;
      for (size_t i = 0; i < section.items.size(); ++i) {
        if (row == selected_) return y;
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
    if (!AnimationsAllowed() || !previousY || !targetY) {
      SyncSelectionAnimationToTarget();
      if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    selectionSettleSeconds_ = motion == SelectionMotion::Hover ? kSelectionHoverSettleSeconds : kSelectionKeyboardSettleSeconds;
    if (!animatingSelection_ || visualSelectedY_ < 0.0f) {
      visualSelectedY_ = *previousY - static_cast<float>(scroll_ - previousScroll);
      lastSelectionFrameQpc_ = NowQpc();
    }
    animatingSelection_ = true;
    StartSelectionAnimationTimer();
  }

  void SelectResult(int next, bool animate, bool ensureVisible, SelectionMotion motion = SelectionMotion::Keyboard) {
    if (flatItems_.empty()) {
      selected_ = 0;
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
    selected_ = next;
    if (ensureVisible) EnsureSelectedVisible();
    if (animate) {
      StartSelectionAnimationFrom(previousY, previousScroll, motion);
    } else {
      SyncSelectionAnimationToTarget();
      if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd_, OBJID_CLIENT, selected_ + 2);
  }

  void UpdateSelectionAnimation(double deltaTime) {
    const auto targetY = SelectedRowTop();
    if (!AnimationsAllowed() || !targetY) {
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
    return visible_ && AnimationsAllowed() && animating_;
  }

  void RequestAnimationFrame() {
    if (!hwnd_ || animationFrameQueued_ || !HasRevealAnimation()) return;
    animationFrameQueued_ = PostMessageW(hwnd_, WM_ANIMATION_FRAME, 0, 0) != FALSE;
  }

  void OnAnimationFrame() {
    animationFrameQueued_ = false;
    if (!HasRevealAnimation()) return;

    const double deltaTime = ConsumeAnimationDeltaSeconds();
    if (animatingSelection_) UpdateSelectionAnimation(deltaTime);

    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);

    if (HasRevealAnimation()) {
      DwmFlush();
      RequestAnimationFrame();
    } else if (animatingSelection_) {
      StartSelectionAnimationTimer();
    }
  }

  void StartSelectionAnimationTimer() {
    if (!hwnd_ || selectionTimerActive_ || !visible_ || !AnimationsAllowed() || !animatingSelection_) return;
    selectionTimerActive_ = SetTimer(hwnd_, TIMER_SELECTION_ANIM, 8, nullptr) != 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void StopSelectionAnimationTimer() {
    if (!selectionTimerActive_ || !hwnd_) {
      selectionTimerActive_ = false;
      lastSelectionFrameQpc_ = 0;
      return;
    }
    KillTimer(hwnd_, TIMER_SELECTION_ANIM);
    selectionTimerActive_ = false;
    lastSelectionFrameQpc_ = 0;
  }

  void OnSelectionAnimationTimer() {
    if (!visible_ || !AnimationsAllowed() || !animatingSelection_) {
      StopSelectionAnimationTimer();
      return;
    }

    UpdateSelectionAnimation(ConsumeSelectionDeltaSeconds());
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!animatingSelection_) StopSelectionAnimationTimer();
  }

  int OverlayWidth() const {
    return std::clamp(settings_.overlayWidth, MIN_OVERLAY_WIDTH, MAX_OVERLAY_WIDTH);
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
    if (CreateGlassSurface(overlaySurface_, hwnd_)) {
      ID2D1DeviceContext* dc = overlaySurface_.dc.Get();
      SetActiveTarget(dc);
      dc->BeginDraw();
      dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));  // fully transparent: desktop content shows through
      hits_.clear();

      // Whole-panel open animation: scale up + rise + fade as one unit. The
      // transparent (DWM-composited) corners let the scale read cleanly.
      const PanelAnim pa = ComputePanelAnim();
      const bool animatingPanel = pa.opacity < 1.0f || pa.scale != 1.0f || pa.dy != 0.0f;
      if (animatingPanel) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const float pivotX = (rc.right - rc.left) * 0.5f;
        dc->SetTransform(
            D2D1::Matrix3x2F::Scale(pa.scale, pa.scale, D2D1::Point2F(pivotX, 0.0f)) *
            D2D1::Matrix3x2F::Translation(0.0f, pa.dy));
        dc->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::IdentityMatrix(), pa.opacity),
            nullptr);
      }

      DrawSearch();

      if (animatingPanel) {
        dc->PopLayer();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
      }
      const HRESULT hr = dc->EndDraw();
      activeRT_ = nullptr;
      activeDC_.Reset();
      if (hr == D2DERR_RECREATE_TARGET) {
        DiscardGlassDevice();
      } else {
        overlaySurface_.swapChain->Present(1, 0);
      }
    }
    EndPaint(hwnd_, &ps);
  }

  void PaintSettings() {
    PAINTSTRUCT ps{};
    BeginPaint(settingsHwnd_, &ps);
    if (CreateGlassSurface(settingsSurface_, settingsHwnd_)) {
      ID2D1DeviceContext* dc = settingsSurface_.dc.Get();
      SetActiveTarget(dc);
      dc->BeginDraw();
      dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
      hits_.clear();
      DrawSettings();
      const HRESULT hr = dc->EndDraw();
      activeRT_ = nullptr;
      activeDC_.Reset();
      if (hr == D2DERR_RECREATE_TARGET) {
        DiscardGlassDevice();
      } else {
        settingsSurface_.swapChain->Present(1, 0);
      }
    }
    EndPaint(settingsHwnd_, &ps);
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
  ComPtr<ID2D1SolidColorBrush> Brush(D2D1_COLOR_F color) {
    auto& byColor = brushCache_[activeRT_];
    const uint32_t key = ColorKey(color);
    if (auto it = byColor.find(key); it != byColor.end()) return it->second;
    ComPtr<ID2D1SolidColorBrush> brush;
    activeRT_->CreateSolidColorBrush(color, brush.GetAddressOf());
    if (brush) byColor.emplace(key, brush);
    return brush;
  }

  void FillRound(RectF rect, float radius, D2D1_COLOR_F color) {
    auto brush = Brush(color);
    activeRT_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius), brush.Get());
  }

  void StrokeRound(RectF rect, float radius, D2D1_COLOR_F color, float width = 1.0f) {
    auto brush = Brush(color);
    activeRT_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), radius, radius), brush.Get(), width);
  }

  enum class ObsidianBackgroundKind {
    Overlay,
    Settings,
  };

  void DrawObsidianBackground(float width, float height, float radius, ObsidianBackgroundKind kind) {
    const RectF panel{0.5f, 0.5f, width - 0.5f, height - 0.5f};
    const bool settings = kind == ObsidianBackgroundKind::Settings;
    feathercast::theme::Color background = settings ? theme_.settingsBackground : theme_.overlayBackground;
    background.a = std::min(background.a, 0.85f);

    FillRound(panel, radius, D2DColor(background));
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

  void DrawSearch() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetWindowScale(hwnd_);
    const float width = static_cast<float>(rc.right - rc.left) / scale;
    const float height = static_cast<float>(rc.bottom - rc.top) / scale;
    const COLORREF accent = ActiveAccent();

    DrawObsidianBackground(width, height, theme_.overlayRadius, ObsidianBackgroundKind::Overlay);

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
    const DisplayItem* detailItem = SelectedMarkdownDetailItem();
    const float detailWidth = detailItem ? MarkdownDetailWidth(width) : 0.0f;
    if (detailWidth <= 0.0f) detailItem = nullptr;
    const float resultsRight = detailItem ? width - detailWidth - 8.0f : width;
    const float viewHeight = resultsBottom - resultsTop;
    const int contentHeight = ResultsContentHeight();
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight - static_cast<int>(viewHeight)));

    float y = resultsTop - static_cast<float>(scroll_);
    int rowIndex = 0;
    activeRT_->PushAxisAlignedClip(D2D1::RectF(0, resultsTop, resultsRight, resultsBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    DrawSelectionPill(resultsRight);
    for (const auto& section : sections_) {
      if (y + kSectionHeaderHeight >= resultsTop && y <= resultsBottom) {
        DrawTextBlock(section.title, {12, y + 8, resultsRight - 12, y + 24}, sectionFormat_.Get(), D2DColor(theme_.sectionText));
      }
      y += kSectionHeaderHeight;
      for (const auto& item : section.items) {
        // 50px row height (was 46px): subtitle no longer clips at the bottom edge.
        RectF rowRect{8, y, resultsRight - 8, y + kResultRowHeight};
        if (rowRect.bottom >= resultsTop && rowRect.top <= resultsBottom) {
          const RowAnim anim = ComputeRowAnim(rowIndex);
          const bool revealing = anim.opacity < 1.0f || anim.dy != 0.0f;
          if (revealing) {
            // Compose the row slide onto the current transform (the panel
            // pop), so both animations layer instead of overwriting each other.
            D2D1_MATRIX_3X2_F base;
            activeRT_->GetTransform(&base);
            activeRT_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, anim.dy) * base);
            activeRT_->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), anim.opacity),
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

    if (animating_ && QpcElapsedMs(animStartQpc_) >= AnimFinishMs()) {
      animating_ = false;
    }

    if (sections_.empty()) {
      DrawTextBlock(appsReady_.load(std::memory_order_acquire) ? L"No results" : L"Loading apps...",
                    {0, 170, width, 210}, centerFormat_.Get(), D2DColor(theme_.textMuted));
    }

    if (showFooter) {
      auto border = Brush(D2DColor(theme_.divider));
      activeRT_->DrawLine(D2D1::Point2F(0, height - 40.0f), D2D1::Point2F(width, height - 40.0f), border.Get(), 1.0f);
      DrawTextBlock(L"FeatherCast", {16, height - 28.0f, 160, height - 12.0f}, footerFormat_.Get(), D2DColor(theme_.textPrimary));
      DrawTextBlock(L"Up/Down Navigate | Enter Open | Tab Actions | Esc Close", {200, height - 28.0f, width - 16.0f, height - 12.0f}, footerRightFormat_.Get(), D2DColor(theme_.textMuted));
    }
  }

  // Open-search reveal animation tuning. Two motions layer together: the whole
  // panel eases in as one unit (a quick fade + subtle scale-up + upward slide),
  // while result rows additionally stagger upward, top-first. Capped stagger
  // keeps the whole reveal short regardless of row count.
  static constexpr double kAnimDurationMs = 200.0;
  static constexpr float kAnimStartScale = 0.97f;
  static constexpr float kAnimSlidePx = 10.0f;
  static constexpr double kAnimBackOvershoot = 1.2;
  static constexpr double kRowDurationMs = 120.0;
  static constexpr double kRowStaggerMs = 18.0;
  static constexpr float kRowSlidePx = 8.0f;
  static constexpr int kRowMaxStaggerRows = 7;

  struct PanelAnim { float opacity; float scale; float dy; };

  PanelAnim ComputePanelAnim() const {
    if (!animating_) return {1.0f, 1.0f, 0.0f};
    double t = QpcElapsedMs(animStartQpc_) / kAnimDurationMs;
    t = std::clamp(t, 0.0, 1.0);
    const double eased = ComputeEaseOutBack(t);
    const double opacity = std::clamp(eased, 0.0, 1.0);
    return {
        static_cast<float>(opacity),
        static_cast<float>(kAnimStartScale + (1.0f - kAnimStartScale) * eased),
        static_cast<float>((1.0 - eased) * kAnimSlidePx),
    };
  }

  static double ComputeEaseOutBack(double t) {
    t = t - 1.0;
    return 1.0 + (kAnimBackOvershoot + 1.0) * std::pow(t, 3.0) + kAnimBackOvershoot * std::pow(t, 2.0);
  }

  struct RowAnim { float opacity; float dy; };

  RowAnim ComputeRowAnim(int rowIndex) const {
    if (!animating_) return {1.0f, 0.0f};
    const double elapsed = QpcElapsedMs(animStartQpc_);
    const double delay = std::min(rowIndex, kRowMaxStaggerRows) * kRowStaggerMs;
    double t = (elapsed - delay) / kRowDurationMs;
    t = std::clamp(t, 0.0, 1.0);
    const double eased = 1.0 - std::pow(1.0 - t, 3.0);  // ease-out cubic
    return {static_cast<float>(eased), static_cast<float>((1.0 - eased) * kRowSlidePx)};
  }

  double AnimFinishMs() const {
    return std::max(kAnimDurationMs, kRowMaxStaggerRows * kRowStaggerMs + kRowDurationMs);
  }

  void DrawSelectionPill(float width) {
    const auto targetY = SelectedRowTop();
    if (!targetY) return;
    if (!AnimationsAllowed() || visualSelectedY_ < 0.0f) {
      visualSelectedY_ = *targetY;
      animatingSelection_ = false;
    }

    const COLORREF accent = ActiveAccent();
    RectF rect{8.0f, visualSelectedY_, width - 8.0f, visualSelectedY_ + kResultRowHeight};
    FillRound(rect, theme_.rowRadius, Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.20f, 1.0f));
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
    hits_.push_back({rowRect, HitType::Result, rowIndex});

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
      std::wstring letter = item.Name().empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(std::towupper(item.Name()[0])));
      DrawTextBlock(letter, {iconX, iconY + 1, iconX + 24, iconY + 24}, centerFormat_.Get(), D2DColor(theme_.textMuted));
    }

    // Unified text start at +52px (was +50 for icon path) to eliminate horizontal jitter.
    DrawTextBlock(item.Name(), {rowRect.left + 52, rowRect.top + 8, rowRect.right - 180, rowRect.top + 28}, rowFormat_.Get(), D2DColor(theme_.textPrimary));
    DrawTextBlock(SourceLabel(item), {rowRect.left + 52, rowRect.top + 29, rowRect.right - 180, rowRect.bottom}, subFormat_.Get(), D2DColor(theme_.textMuted));
    if (selected && rowRect.right - rowRect.left > 520.0f) {
      DrawTextBlock(ActionHint(item), {rowRect.right - 330, rowRect.top + 16, rowRect.right - 10, rowRect.bottom}, footerRightFormat_.Get(), D2DColor(theme_.textMuted));
    }
  }

  std::wstring SourceLabel(const DisplayItem& item) const {
    if (item.isCalculator || item.isConversion) return item.commandDetail;
    if (item.isWebSearch) return item.commandDetail;
    if (item.isExtension) return item.commandDetail;
    if (item.isSnippet) return L"Snippet - " + item.snippet.keyword;
    if (item.isClipboard) return L"Clipboard History";
    if (item.isRunCommand) return item.commandDetail;
    if (item.isSymbol) return L"Symbol - paste";
    if (item.isCommand || item.isAction) return item.commandDetail;
    if (item.isWindow) return item.window.processName.empty() ? L"Open window" : L"Open window - " + item.window.processName;
    if (item.app.source == L"shortcut") return L"Desktop app";
    if (item.app.source == L"quicklink") return L"Quicklink";
    if (item.app.source == L"file") return item.app.path.empty() ? L"File or folder" : item.app.path;
    if (item.app.source == L"system-folder") return item.app.path.empty() ? L"System folder" : item.app.path;
    if (item.app.source == L"alias") return L"System app";
    if (item.app.source == L"appx") return L"Store/System app";
    return L"Start menu app";
  }

  std::wstring ActionHint(const DisplayItem& item) const {
    if (item.isCalculator || item.isConversion) return L"Enter Copy";
    if (item.isWebSearch) return L"Enter Open";
    if (item.isExtension) return L"Enter Run";
    if (item.isSnippet || item.isClipboard) return L"Enter Paste";
    if (item.isRunCommand) return item.runCommand.kind == feathercast::run_command::Kind::OpenTarget ? L"Enter Open" : L"Enter Run";
    if (item.isSymbol) return L"Enter Paste";
    if (item.isCommand) return L"Enter Run";
    if (item.isAction) return L"Enter Apply";
    if (item.isWindow) return L"Enter Switch";
    return L"Enter Open | Ctrl+Shift Admin | Ctrl+K Actions";
  }

  static constexpr float kSettTop = 72.0f;
  static constexpr float kSettSection = 34.0f;
  static constexpr float kSettRow = 60.0f;
  static constexpr float kSettShortcut = 98.0f;
  static constexpr float kSettMaint = 58.0f;
  static constexpr float kSettBottom = 22.0f;

  int SettingsContentHeight() const {
    float h = kSettTop;
    h += kSettSection + kSettShortcut;            // Shortcut
    h += kSettSection + 4 * kSettRow;             // General
    h += kSettSection + 4 * kSettRow;             // Results
    h += kSettSection + 5 * kSettRow + 2 * kSettMaint; // Privacy & local data
    h += kSettSection + kSettRow + kSettMaint;  // Extensions
    h += kSettSection + kSettRow + (settings_.syncAccentColor ? 0.0f : 48.0f);  // Appearance
    h += kSettSection + kSettMaint;               // Maintenance
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
      HitType::RecordShortcut,
      HitType::StartupToggle,
      HitType::UpdateChecksToggle,
      HitType::CompactToggle,
      HitType::AnimationsToggle,
      HitType::ShowWindowsToggle,
      HitType::ShowStoreAppsToggle,
      HitType::OverlayWidthDown,
      HitType::OverlayWidthUp,
      HitType::MaxResultsDown,
      HitType::MaxResultsUp,
      HitType::ClipboardHistoryToggle,
      HitType::ClipboardLimitDown,
      HitType::ClipboardLimitUp,
      HitType::FileIndexToggle,
      HitType::FileIndexLimitDown,
      HitType::FileIndexLimitUp,
      HitType::AddFileRoot,
      HitType::ClearFileRoots,
      HitType::DiagnosticsToggle,
      HitType::ClearClipboardData,
      HitType::ClearFileIndexData,
      HitType::OpenLocalDataFolder,
      HitType::ReloadExtensions,
      HitType::OpenPluginsFolder,
      HitType::AccentToggle,
      HitType::ClearRecents,
      HitType::ClearIconCache,
      HitType::CheckUpdates,
      HitType::CloseSettings,
    };
    auto shortcutAction = order.begin() + 1;
    if (!pendingShortcut_.empty()) {
      shortcutAction = order.insert(shortcutAction, HitType::SaveShortcut) + 1;
    }
    if (!settings_.shortcut.empty() && settings_.shortcut != L"none") {
      order.insert(shortcutAction, HitType::ClearShortcut);
    }
    if (!settings_.syncAccentColor) {
      const auto accent = std::find(order.begin(), order.end(), HitType::AccentToggle);
      order.insert(accent + 1, HitType::AccentColor);
    }
    return order;
  }

  D2D1_COLOR_F SettWhite() const { return D2DColor(theme_.textPrimary); }
  D2D1_COLOR_F SettGray() const { return D2DColor(theme_.textMuted); }

  float DrawSettingsSection(float y, const std::wstring& title, float width) {
    DrawTextBlock(title, {24, y + 12, width - 24, y + 30}, sectionFormat_.Get(), D2DColor(theme_.sectionText));
    auto border = Brush(D2DColor(theme_.divider));
    activeRT_->DrawLine(D2D1::Point2F(24, y + kSettSection - 2), D2D1::Point2F(width - 24, y + kSettSection - 2), border.Get(), 1);
    return y + kSettSection;
  }

  void DrawSettingRowLabel(float y, float rowH, const std::wstring& label, const std::wstring& desc, float labelRight) {
    DrawTextBlock(label, {24, y + rowH / 2 - 20, labelRight, y + rowH / 2}, labelFormat_.Get(), SettWhite());
    DrawTextBlock(desc, {24, y + rowH / 2, labelRight, y + rowH / 2 + 20}, bodyFormat_.Get(), SettGray());
  }

  void DrawSwitch(float y, float rowH, bool on, HitType type, float width) {
    const COLORREF accent = ActiveAccent();
    const float right = width - 24.0f;
    const float left = right - 46.0f;
    const float h = 26.0f;
    const float top = y + (rowH - h) / 2.0f;
    RectF track{left, top, right, top + h};
    const bool hover = SettHover(type);
    FillRound(track, 13, on ? D2DColor(accent) : (hover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.surface)));
    if (SettFocused(type)) StrokeRound(track, 13, D2DColor(accent), 2.0f);
    const float knobX = on ? track.right - 22.0f : track.left + 4.0f;
    FillRound({knobX, top + 4, knobX + 18, top + 22}, 9, D2D1::ColorF(1, 1, 1));
    hits_.push_back({{16.0f, y, width - 16.0f, y + rowH}, type});
  }

  void DrawStepper(float y, float rowH, const std::wstring& value, HitType down, HitType up, float width) {
    const float cy = y + rowH / 2.0f;
    RectF plus{width - 24 - 36, cy - 17, width - 24, cy + 17};
    RectF minus{plus.left - 8 - 36, cy - 17, plus.left - 8, cy + 17};
    DrawSettingsButton(minus, L"-", down);
    DrawSettingsButton(plus, L"+", up);
    DrawTextBlock(value, {minus.left - 96, cy - 11, minus.left - 12, cy + 11}, footerRightFormat_.Get(), SettWhite());
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
    auto topBorder = Brush(D2DColor(theme_.border));
    activeRT_->DrawLine(D2D1::Point2F(0, 56), D2D1::Point2F(width, 56), topBorder.Get(), 1);

    // Keep the scroll offset within range (e.g. after a DPI/height change) and
    // clip the scrollable content so it never paints over the fixed title bar.
    const float maxScroll = std::max(0.0f, static_cast<float>(SettingsContentHeight()) - height);
    settingsScroll_ = std::clamp(settingsScroll_, 0.0f, maxScroll);
    activeRT_->PushAxisAlignedClip(D2D1::RectF(0, 57, width, height), D2D1_ANTIALIAS_MODE_ALIASED);

    float y = kSettTop - settingsScroll_;

    // ---- Shortcut ----
    y = DrawSettingsSection(y, L"GLOBAL SHORTCUT", width);
    {
      const float rowH = kSettShortcut;
      const std::wstring current = settings_.shortcut.empty() || settings_.shortcut == L"none" ? L"None" : settings_.shortcut;
      DrawTextBlock(L"Open FeatherCast", {24, y + 12, width - 24, y + 32}, labelFormat_.Get(), SettWhite());
      DrawTextBlock(L"Current: " + current + L". At least one modifier required.", {24, y + 33, width - 24, y + 51}, bodyFormat_.Get(), SettGray());
      const float btnTop = y + 56;
      RectF record{24, btnTop, width - 130, btnTop + 38};
      const bool recHover = SettHover(HitType::RecordShortcut);
      FillRound(record, theme_.controlRadius, recording_ ? Mix(accent, ColorRefFromTheme(theme_.selectedBase), 0.28f) : (recHover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.surface)));
      StrokeRound(record, theme_.controlRadius, recording_ ? D2DColor(accent) : D2DColor(theme_.border));
      if (SettFocused(HitType::RecordShortcut)) {
        StrokeRound(record, theme_.controlRadius, D2DColor(accent), 2.0f);
      }
      hits_.push_back({record, HitType::RecordShortcut});
      const std::wstring recordText = recording_ ? L"Press a key..." : (!pendingShortcut_.empty() ? pendingShortcut_ : L"Record new shortcut");
      DrawTextBlock(recordText, {record.left + 12, record.top + 10, record.right - 12, record.bottom}, bodyFormat_.Get(), recording_ ? D2DColor(accent) : SettWhite());
      if (!pendingShortcut_.empty()) {
        RectF save{width - 118, btnTop, width - 24, btnTop + 38};
        FillRound(save, theme_.controlRadius, D2DColor(accent));
        hits_.push_back({save, HitType::SaveShortcut});
        DrawTextBlock(L"Save", save, centerFormat_.Get(), D2D1::ColorF(1, 1, 1));
      } else if (!settings_.shortcut.empty() && settings_.shortcut != L"none") {
        RectF clearBtn{width - 118, btnTop, width - 24, btnTop + 38};
        StrokeRound(clearBtn, theme_.controlRadius, D2DColor(theme_.danger));
        hits_.push_back({clearBtn, HitType::ClearShortcut});
        DrawTextBlock(L"Clear", clearBtn, centerFormat_.Get(), D2DColor(theme_.danger));
      }
      y += rowH;
    }

    // ---- General ----
    y = DrawSettingsSection(y, L"GENERAL", width);
    DrawSettingRowLabel(y, kSettRow, L"Start on Startup", L"Launch FeatherCast when you log into Windows.", width - 90);
    DrawSwitch(y, kSettRow, settings_.startOnStartup, HitType::StartupToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Automatic Update Checks", L"Check GitHub Releases once per day.", width - 90);
    DrawSwitch(y, kSettRow, settings_.updateChecksEnabled, HitType::UpdateChecksToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Compact Mode", L"Show only the search bar at rest; results expand below.", width - 90);
    DrawSwitch(y, kSettRow, settings_.compactMode, HitType::CompactToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Enable Animations", L"Animate results when the search opens.", width - 90);
    DrawSwitch(y, kSettRow, settings_.animationsEnabled, HitType::AnimationsToggle, width);
    y += kSettRow;

    // ---- Results ----
    y = DrawSettingsSection(y, L"RESULTS", width);
    DrawSettingRowLabel(y, kSettRow, L"Open Window Results", L"Include currently open windows in search results.", width - 90);
    DrawSwitch(y, kSettRow, settings_.showOpenWindows, HitType::ShowWindowsToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Store/System Apps", L"Include AppsFolder, Store, and system alias entries.", width - 90);
    DrawSwitch(y, kSettRow, settings_.showStoreApps, HitType::ShowStoreAppsToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Overlay Width", L"Width of the search overlay window.", width - 200);
    DrawStepper(y, kSettRow, std::to_wstring(OverlayWidth()) + L" px", HitType::OverlayWidthDown, HitType::OverlayWidthUp, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Max Results", L"Maximum number of results to show.", width - 200);
    DrawStepper(y, kSettRow, std::to_wstring(std::clamp(settings_.maxResults, MIN_RESULTS, MAX_RESULT_SETTING)), HitType::MaxResultsDown, HitType::MaxResultsUp, width);
    y += kSettRow;

    // ---- Privacy ----
    y = DrawSettingsSection(y, L"PRIVACY", width);
    DrawSettingRowLabel(y, kSettRow, L"Clipboard History",
                        L"Store copied text locally for launcher search and paste.", width - 90);
    DrawSwitch(y, kSettRow, settings_.clipboardHistoryEnabled, HitType::ClipboardHistoryToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Clipboard Retention",
                        L"Maximum number of text entries retained locally.", width - 200);
    DrawStepper(y, kSettRow, std::to_wstring(ClipboardHistoryLimit()),
                HitType::ClipboardLimitDown, HitType::ClipboardLimitUp, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Files & Folders Index",
                        settings_.fileIndexRoots.empty()
                            ? L"Using Desktop, Documents, and Downloads."
                            : std::to_wstring(settings_.fileIndexRoots.size()) + L" custom folder(s).",
                        width - 90);
    DrawSwitch(y, kSettRow, settings_.fileIndexEnabled, HitType::FileIndexToggle, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"File Index Limit",
                        L"Maximum number of files and folders stored locally.", width - 200);
    DrawStepper(y, kSettRow, std::to_wstring(FileIndexLimit()),
                HitType::FileIndexLimitDown, HitType::FileIndexLimitUp, width);
    y += kSettRow;
    DrawSettingRowLabel(y, kSettRow, L"Diagnostics",
                        L"Write bounded troubleshooting logs without queries or clipboard text.", width - 90);
    DrawSwitch(y, kSettRow, settings_.diagnosticsEnabled, HitType::DiagnosticsToggle, width);
    y += kSettRow;
    {
      const float btnW = (width - 48 - 12) / 2.0f;
      const float btnTop = y + (kSettMaint - 38) / 2.0f;
      DrawSettingsButton({24, btnTop, 24 + btnW, btnTop + 38}, L"Add Indexed Folder", HitType::AddFileRoot);
      DrawSettingsButton({36 + btnW, btnTop, width - 24, btnTop + 38}, L"Use Default Folders", HitType::ClearFileRoots);
      y += kSettMaint;
    }
    {
      const float btnW = (width - 48 - 24) / 3.0f;
      const float btnTop = y + (kSettMaint - 38) / 2.0f;
      DrawSettingsButton({24, btnTop, 24 + btnW, btnTop + 38}, L"Delete Clipboard Data", HitType::ClearClipboardData);
      DrawSettingsButton({36 + btnW, btnTop, 24 + 2 * btnW + 12, btnTop + 38}, L"Delete File Index", HitType::ClearFileIndexData);
      DrawSettingsButton({width - 24 - btnW, btnTop, width - 24, btnTop + 38}, L"Open Local Data", HitType::OpenLocalDataFolder);
      y += kSettMaint;
    }

    // ---- Extensions ----
    y = DrawSettingsSection(y, L"EXTENSIONS", width);
    {
      const auto health = extensions_.Health();
      const size_t available = static_cast<size_t>(std::count_if(
          health.begin(), health.end(),
          [](const feathercast::extensions::PluginHealth& plugin) {
            return plugin.available;
          }));
      DrawSettingRowLabel(
          y, kSettRow, L"Installed Native Extensions",
          std::to_wstring(available) + L" of " + std::to_wstring(health.size()) +
              L" available. Only install extensions you trust.",
          width - 24);
      y += kSettRow;
      const float btnW = (width - 48 - 12) / 2.0f;
      const float btnTop = y + (kSettMaint - 38) / 2.0f;
      DrawSettingsButton({24, btnTop, 24 + btnW, btnTop + 38},
                         L"Reload Extensions", HitType::ReloadExtensions);
      DrawSettingsButton({36 + btnW, btnTop, width - 24, btnTop + 38},
                         L"Open Plugins Folder", HitType::OpenPluginsFolder);
      y += kSettMaint;
    }

    // ---- Appearance ----
    y = DrawSettingsSection(y, L"APPEARANCE", width);
    DrawSettingRowLabel(y, kSettRow, L"Sync Accent Color", settings_.syncAccentColor ? L"Match the Windows accent color automatically." : L"Using a custom accent color.", width - 90);
    DrawSwitch(y, kSettRow, settings_.syncAccentColor, HitType::AccentToggle, width);
    y += kSettRow;
    if (!settings_.syncAccentColor) {
      RectF colorBox{24, y, 220, y + 36};
      const bool colorHover = SettHover(HitType::AccentColor);
      FillRound(colorBox, theme_.controlRadius, D2DColor(theme_.surface));
      StrokeRound(colorBox, theme_.controlRadius, colorHover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.border));
      FillRound({36, y + 9, 60, y + 27}, 5, D2DColor(ColorRefFromHex(settings_.customAccentColor)));
      DrawTextBlock(L"Pick color  " + settings_.customAccentColor, {72, y + 9, 212, y + 29}, bodyFormat_.Get(), SettWhite());
      hits_.push_back({colorBox, HitType::AccentColor});
      y += 48;
    }

    // ---- Maintenance ----
    y = DrawSettingsSection(y, L"MAINTENANCE", width);
    {
      const float btnW = (width - 48 - 24) / 3.0f;
      const float btnTop = y + (kSettMaint - 38) / 2.0f;
      DrawSettingsButton({24, btnTop, 24 + btnW, btnTop + 38}, L"Clear Recents", HitType::ClearRecents);
      DrawSettingsButton({24 + btnW + 12, btnTop, 24 + 2 * btnW + 12, btnTop + 38}, L"Clear Icon Cache", HitType::ClearIconCache);
      DrawSettingsButton({width - 24 - btnW, btnTop, width - 24, btnTop + 38}, L"Check Updates", HitType::CheckUpdates);
    }

    activeRT_->PopAxisAlignedClip();

    // Scrollbar thumb, shown only when the content overflows the window.
    if (maxScroll > 0.0f) {
      const float trackTop = 60.0f;
      const float trackBottom = height - 8.0f;
      const float trackH = trackBottom - trackTop;
      const float contentH = static_cast<float>(SettingsContentHeight());
      const float thumbH = std::max(36.0f, trackH * (height / contentH));
      const float thumbY = trackTop + (trackH - thumbH) * (settingsScroll_ / maxScroll);
      FillRound({width - 7, thumbY, width - 4, thumbY + thumbH}, 1.5f, D2DColor(theme_.textDim));
    }
  }

  void DrawSettingsButton(RectF rect, const std::wstring& text, HitType type) {
    const bool hover = SettHover(type);
    FillRound(rect, theme_.controlRadius, hover ? D2DColor(theme_.surfaceHover) : D2DColor(theme_.surface));
    StrokeRound(rect, theme_.controlRadius,
                SettFocused(type) ? D2DColor(ActiveAccent()) :
                (hover ? D2DColor(theme_.textMuted) : D2DColor(theme_.border)),
                SettFocused(type) ? 2.0f : 1.0f);
    DrawTextBlock(text, rect, centerFormat_.Get(), SettWhite());
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
  }

  ComPtr<ID2D1Bitmap> IconBitmap(const std::wstring& key) {
    if (key.empty() || !overlaySurface_.dc) return nullptr;
    if (auto cached = CachedIconBitmap(key)) return cached;

    const auto png = IconCachePath(key);
    std::error_code ec;
    if (std::filesystem::exists(png, ec)) {
      auto bitmap = LoadBitmapFromFile(png);
      if (bitmap) {
        StoreIconBitmap(key, bitmap);
        return bitmap;
      }
    }

    QueueIcon(key);
    return nullptr;
  }

  // Hand the key to the persistent icon worker pool. No thread is created here,
  // so repeated searches no longer accumulate threads.
  void QueueIcon(const std::wstring& key) {
    {
      std::lock_guard lock(iconQueueMutex_);
      if (pendingIcons_.contains(key)) return;
      pendingIcons_.insert(key);
      iconJobs_.push_back(key);
    }
    iconCv_.notify_one();
  }

  void StartIconWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = std::min<size_t>(3, std::max<unsigned>(2, hw / 2));
    std::lock_guard threadLock(workerThreadsMutex_);
    for (size_t i = 0; i < workers; ++i) {
      iconThreads_.emplace_back([this](std::stop_token stopToken) { IconWorkerLoop(stopToken); });
    }
  }

  void IconWorkerLoop(std::stop_token stopToken) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
      std::wstring key;
      {
        std::unique_lock lock(iconQueueMutex_);
        iconCv_.wait(lock, [&] { return !iconJobs_.empty() || stopThreads_ || stopToken.stop_requested(); });
        if (stopThreads_ || stopToken.stop_requested()) break;
        key = std::move(iconJobs_.front());
        iconJobs_.pop_front();
      }

      if (!stopThreads_ && !stopToken.stop_requested()) ResolveIconToCache(key);
      {
        std::lock_guard lock(iconQueueMutex_);
        pendingIcons_.erase(key);
      }
      if (!stopThreads_ && !stopToken.stop_requested() &&
          !iconReadyMessageQueued_.exchange(true, std::memory_order_acq_rel)) {
        if (!PostMessageW(hwnd_, WM_ICON_READY, 0, 0)) {
          iconReadyMessageQueued_.store(false, std::memory_order_release);
        }
      }
    }
    CoUninitialize();
  }

  ComPtr<ID2D1Bitmap> LoadBitmapFromFile(const std::filesystem::path& path) {
    if (!wicFactory_ || !overlaySurface_.dc) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf()))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    overlaySurface_.dc->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf());
    return bitmap;
  }

  bool ResolveIconToCache(const std::wstring& key) {
    const auto png = IconCachePath(key);
    std::error_code ec;
    if (std::filesystem::exists(png, ec)) return true;
    UniqueBitmap bitmap(CreateShellBitmap(key));
    if (!bitmap) return false;
    auto temporary = png;
    temporary += L".tmp-" + std::to_wstring(GetCurrentThreadId());
    std::filesystem::remove(temporary, ec);
    if (!SaveHBitmapPng(bitmap.get(), temporary)) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    if (!MoveFileExW(temporary.c_str(), png.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(temporary, ec);
      return std::filesystem::exists(png, ec);
    }
    return true;
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

  bool SaveHBitmapPng(HBITMAP bitmap, const std::filesystem::path& path) {
    if (!wicFactory_ || !bitmap) return false;
    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory_->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, wicBitmap.GetAddressOf()))) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wicFactory_->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) return false;
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
    if (vk == VK_ESCAPE) {
      if (browseView_ != BrowseView::None) {
        ExitBrowseView();
      } else if (actionMode_) {
        ExitActionMode();
      } else {
        HideOverlay(true);
      }
      return;
    }

    if (view_ != View::Search) return;
    const bool control = ModifierPressed(VK_CONTROL);
    const bool shift = ModifierPressed(VK_SHIFT);

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
        selected_ = 0;
        scroll_ = 0;
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return;
    }
    if (control && vk == 'V') {
      if (const auto text = ReadClipboardText()) {
        InsertQueryText(*text);
        selected_ = 0;
        scroll_ = 0;
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return;
    }

    if (vk == 'K' && ModifierPressed(VK_CONTROL)) {
      if (!actionMode_ && browseView_ == BrowseView::None &&
          selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) {
        EnterActionMode(flatItems_[selected_]);
      }
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
                 selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) {
        EnterActionMode(flatItems_[selected_]);
      }
    } else if (vk == VK_TAB) {
      if (browseView_ == BrowseView::None && selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size()))
        EnterActionMode(flatItems_[selected_]);
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
      if (selected_ >= 0 && selected_ < static_cast<int>(flatItems_.size())) {
        const bool admin = ModifierPressed(VK_CONTROL) && ModifierPressed(VK_SHIFT);
        Activate(flatItems_[selected_], admin);
      }
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
        selected_ = 0;
        scroll_ = 0;
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
        selected_ = 0;
        scroll_ = 0;
        SyncSelectionAnimationToTarget();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
    }
  }

  void OnChar(wchar_t ch) {
    if (view_ != View::Search) return;
    if (ch >= 32 && ch != 127) {
      ClampCaret();
      InsertQueryText(std::wstring(1, ch));
      selected_ = 0;
      scroll_ = 0;
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
          if (rowTop - scroll_ < 0) scroll_ = rowTop;
          else if (rowBottom - scroll_ > visible) scroll_ = rowBottom - visible;
          scroll_ = std::max(0, scroll_);
          return;
        }
        y += static_cast<int>(kResultRowStride);
        ++row;
      }
    }
  }

  void OnMouseMove(float x, float y) {
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

  void OnSettingsClick(float x, float y) {
    for (const auto& hit : hits_) {
      if (!PointInRect(hit.rect, x, y)) continue;
      if (hit.type == HitType::CloseSettings) {
        HideSettings();
        return;
      }
      HandleSettingsHit(hit.type);
      return;
    }
  }

  void OnClick(float x, float y) {
    if (view_ == View::Search) {
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

    for (const auto& hit : hits_) {
      if (!PointInRect(hit.rect, x, y)) continue;
      if (hit.type == HitType::Result) {
        if (hit.index >= 0 && hit.index < static_cast<int>(flatItems_.size())) Activate(flatItems_[hit.index], false);
        return;
      }
      if (hit.type == HitType::Gear) {
        OpenSettings();
        return;
      }
    }
  }

  void OnRightClick(float x, float y) {
    if (actionMode_ || browseView_ != BrowseView::None) return;
    for (const auto& hit : hits_) {
      if (hit.type != HitType::Result || !PointInRect(hit.rect, x, y)) continue;
      if (hit.index >= 0 && hit.index < static_cast<int>(flatItems_.size())) {
        SelectResult(hit.index, false, true);
        EnterActionMode(flatItems_[hit.index]);
      }
      return;
    }
  }

  void ResizeSettingsWindow() {
    if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) return;
    RECT rc{};
    GetWindowRect(settingsHwnd_, &rc);
    const float scale = GetWindowScale(settingsHwnd_);
    const int width = rc.right - rc.left;
    int height = SettingsContentHeight();
    HMONITOR monitor = MonitorFromWindow(settingsHwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    height = std::min(height, static_cast<int>((mi.rcWork.bottom - mi.rcWork.top - 40) / scale));
    const int physicalHeight = static_cast<int>(height * scale);
    SetWindowPos(settingsHwnd_, HWND_NOTOPMOST, rc.left, rc.top, width, physicalHeight, SWP_NOACTIVATE);
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
      if (hit.rect.top < 64.0f) settingsScroll_ = std::max(0.0f, settingsScroll_ - (64.0f - hit.rect.top));
      else if (hit.rect.bottom > height - 8.0f) settingsScroll_ += hit.rect.bottom - (height - 8.0f);
      settingsScroll_ = std::clamp(settingsScroll_, 0.0f, SettingsMaxScroll());
      return;
    }
  }

  void HandleSettingsKeyDown(UINT vk) {
    if (recording_) return;
    const auto order = SettingsFocusOrder();
    if (order.empty()) return;
    if (vk == VK_TAB) {
      const int direction = ModifierPressed(VK_SHIFT) ? -1 : 1;
      settingsFocusIndex_ = (settingsFocusIndex_ + direction + static_cast<int>(order.size())) %
                            static_cast<int>(order.size());
      EnsureSettingsFocusVisible();
      InvalidateRect(settingsHwnd_, nullptr, FALSE);
      NotifyWinEvent(EVENT_OBJECT_FOCUS, settingsHwnd_, OBJID_CLIENT, settingsFocusIndex_ + 1);
      return;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
      const HitType focused = order[static_cast<size_t>(std::clamp(
          settingsFocusIndex_, 0, static_cast<int>(order.size()) - 1))];
      if (focused == HitType::CloseSettings) HideSettings();
      else HandleSettingsHit(focused);
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
    switch (type) {
      case HitType::RecordShortcut:
        pendingShortcut_.clear();
        shortcutRecorder_.Reset();
        recording_ = true;
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
          std::lock_guard lock(storageMutex_);
          ReloadClipboardHistoryFromStorage();
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
          std::lock_guard lock(dataMutex_);
          fileIndex_.clear();
        }
        PersistSettings();
        StartAppDiscovery();
        RequestSearch();
        break;
      case HitType::FileIndexLimitDown:
        settings_.fileIndexMaxEntries = std::clamp(
            settings_.fileIndexMaxEntries - 1000,
            MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
        PersistSettings();
        if (settings_.fileIndexEnabled) StartAppDiscovery();
        break;
      case HitType::FileIndexLimitUp:
        settings_.fileIndexMaxEntries = std::clamp(
            settings_.fileIndexMaxEntries + 1000,
            MIN_FILE_INDEX_ENTRIES, MAX_FILE_INDEX_ENTRIES);
        PersistSettings();
        if (settings_.fileIndexEnabled) StartAppDiscovery();
        break;
      case HitType::AddFileRoot:
        if (const auto folder = PickIndexedFolder()) {
          const std::wstring normalized = Lower(*folder);
          const bool exists = std::any_of(
              settings_.fileIndexRoots.begin(), settings_.fileIndexRoots.end(),
              [&](const std::wstring& root) { return Lower(root) == normalized; });
          if (!exists) settings_.fileIndexRoots.push_back(*folder);
          PersistSettings();
          if (settings_.fileIndexEnabled) StartAppDiscovery();
          ResizeSettingsWindow();
        }
        break;
      case HitType::ClearFileRoots:
        settings_.fileIndexRoots.clear();
        PersistSettings();
        if (settings_.fileIndexEnabled) StartAppDiscovery();
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
        launchExecutor_.Submit([this](std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          extensions_.Reload();
          if (!stopToken.stop_requested() && !stopThreads_) {
            PostMessageW(hwnd_, WM_REBUILD_RESULTS, 0, 0);
          }
        });
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
      case HitType::AnimationsToggle:
        settings_.animationsEnabled = !settings_.animationsEnabled;
        PersistSettings();
        break;
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
    if (view_ != View::Search || sections_.empty()) return;
    const int previousScroll = scroll_;
    scroll_ -= delta / WHEEL_DELTA * 72;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float scale = GetWindowScale(hwnd_);
    const int visible = static_cast<int>((rc.bottom - rc.top) / scale) - static_cast<int>(kResultsTop) - (settings_.compactMode ? 0 : 36);
    scroll_ = std::clamp(scroll_, 0, std::max(0, ResultsContentHeight() - visible));
    if (scroll_ != previousScroll) SyncSelectionAnimationToTarget();
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
    return std::max(0.0f, static_cast<float>(SettingsContentHeight()) - viewHeight);
  }

  void OnSettingsMouseWheel(int delta) {
    const float maxScroll = SettingsMaxScroll();
    if (maxScroll <= 0.0f) return;
    settingsScroll_ -= static_cast<float>(delta) / WHEEL_DELTA * 60.0f;
    settingsScroll_ = std::clamp(settingsScroll_, 0.0f, maxScroll);
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
        selected_ = 0;
        scroll_ = 0;
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

    if (response->closeOverlay) HideOverlay(false);
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

  void Activate(DisplayItem item, bool asAdmin) {
    DebugLaunchLog(L"Activate: isSnippet=" + std::to_wstring(item.isSnippet) +
                   L" isClipboard=" + std::to_wstring(item.isClipboard) +
                   L" isSymbol=" + std::to_wstring(item.isSymbol) +
                   L" isRunCommand=" + std::to_wstring(item.isRunCommand) +
                   L" isCalculator=" + std::to_wstring(item.isCalculator) +
                   L" isWebSearch=" + std::to_wstring(item.isWebSearch) +
                   L" isExtension=" + std::to_wstring(item.isExtension) +
                   L" isCommand=" + std::to_wstring(item.isCommand) +
                   L" isAction=" + std::to_wstring(item.isAction) +
                   L" isWindow=" + std::to_wstring(item.isWindow) +
                   L" appName='" + item.app.name + L"' appId='" + item.app.id + L"'" +
                   L" appLaunchTarget='" + item.app.launchTarget + L"'" +
                   L" appType=" + std::to_wstring(static_cast<int>(item.app.launchType)));

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

    if (item.isRunCommand) {
      HideOverlay(false);
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
        HideOverlay(false);
        ShowTrayNotification(L"FeatherCast", L"Copied to clipboard.");
      } else {
        ShowTrayNotification(L"FeatherCast", L"Could not copy to clipboard.");
      }
      return;
    }

    if (item.isWebSearch) {
      HideOverlay(false);
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
      HideOverlay(false);
      FocusWindow(item.window.hwnd);
      return;
    }

    HideOverlay(false);
    auto appPtr = std::make_shared<AppEntry>(item.app);
    if (!launchExecutor_.Submit([this, appPtr, asAdmin, id = PrimaryAppId(item.app)](std::stop_token stopToken) {
      if (stopToken.stop_requested()) return;
      DebugLaunchLog(L"Lambda: name='" + appPtr->name + L"' target='" + appPtr->launchTarget + L"'");
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

  void ClearClipboardHistoryData() {
    const int choice = MessageBoxW(settingsHwnd_ ? settingsHwnd_ : hwnd_,
                                   L"Delete all stored clipboard history?",
                                   L"FeatherCast Privacy",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES) return;
    {
      std::lock_guard lock(dataMutex_);
      clipboardHistory_.clear();
    }
    {
      std::lock_guard lock(storageMutex_);
      if (storage_.IsOpen()) storage_.ClearClipboardHistory();
    }
    MarkSearchDataChanged();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void ClearFileIndexData() {
    const int choice = MessageBoxW(settingsHwnd_ ? settingsHwnd_ : hwnd_,
                                   L"Delete all stored file and folder index data?",
                                   L"FeatherCast Privacy",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES) return;
    {
      std::lock_guard lock(dataMutex_);
      fileIndex_.clear();
    }
    {
      std::lock_guard lock(storageMutex_);
      if (storage_.IsOpen()) storage_.ClearFileIndex();
    }
    MarkSearchDataChanged();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  bool ConfirmSystemAction(const wchar_t* message) {
    return MessageBoxW(hwnd_, message, L"FeatherCast System Action",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
  }

  void ExecuteCommand(CommandKind command) {
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
        selected_ = 0;
        scroll_ = 0;
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
        HideOverlay(false);
        return;
      case CommandKind::OpenLocalDataFolder:
        launchExecutor_.Submit([path = LocalDataPath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          CoUninitialize();
        });
        HideOverlay(false);
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
        HideOverlay(false);
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
        HideOverlay(false);
        return;
      case CommandKind::ReloadSnippets:
        {
          auto snippets = LoadSnippets();
          std::lock_guard lock(dataMutex_);
          snippets_ = std::move(snippets);
        }
        MarkSearchDataChanged();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::OpenThemeFile:
        feathercast::theme::WriteDefaultTheme(ThemePath());
        launchExecutor_.Submit([path = ThemePath()](std::stop_token) {
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          CoUninitialize();
        });
        HideOverlay(false);
        return;
      case CommandKind::ReloadTheme:
        ReloadTheme();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      case CommandKind::LockPC:
        HideOverlay(false);
        LockWorkStation();
        return;
      case CommandKind::SleepPC:
        if (!ConfirmSystemAction(L"Put this computer to sleep now?")) return;
        HideOverlay(false);
        SetSuspendState(FALSE, FALSE, FALSE);
        return;
      case CommandKind::MuteAudio:
        ToggleDefaultAudioMute();
        HideOverlay(false);
        return;
      case CommandKind::ShutDown:
        if (!ConfirmSystemAction(L"Shut down this computer now?")) return;
        HideOverlay(false);
        if (EnableShutdownPrivilege()) {
          ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        }
        return;
      case CommandKind::RestartPC:
        if (!ConfirmSystemAction(L"Restart this computer now?")) return;
        HideOverlay(false);
        if (EnableShutdownPrivilege()) {
          ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        }
        return;
      case CommandKind::EmptyRecycleBin:
        if (!ConfirmSystemAction(L"Permanently delete all Recycle Bin contents?")) return;
        HideOverlay(false);
        SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        return;
    }
  }

  void ExecuteAction(DisplayItem item) {
    if (item.actionTargetIsWindow) {
      HWND target = item.actionWindow.hwnd;
      if (!target || !IsWindow(target)) {
        actionMode_ = false;
        RefreshWindowsAsync();
        RequestSearch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }

      switch (item.action) {
        case ActionKind::Switch:
          HideOverlay(false);
          FocusWindow(target);
          return;
        case ActionKind::Minimize:
          ShowWindowAsync(target, SW_MINIMIZE);
          HideOverlay(false);
          return;
        case ActionKind::MaximizeRestore:
          if (IsIconic(target) || IsZoomed(target)) ShowWindowAsync(target, SW_RESTORE);
          else ShowWindowAsync(target, SW_MAXIMIZE);
          HideOverlay(false);
          return;
        case ActionKind::CloseWindow:
          PostMessageW(target, WM_CLOSE, 0, 0);
          HideOverlay(false);
          return;
        default:
          return;
      }
    }

    const AppEntry& app = item.actionApp;
    const std::wstring id = PrimaryAppId(app);
    switch (item.action) {
      case ActionKind::Open:
      case ActionKind::RunAsAdmin: {
        HideOverlay(false);
        auto appPtr = std::make_shared<AppEntry>(app);
        if (!launchExecutor_.Submit([this, appPtr, runAsAdmin = (item.action == ActionKind::RunAsAdmin), id](std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          DebugLaunchLog(L"LambdaAction: name='" + appPtr->name + L"' target='" + appPtr->launchTarget + L"'");
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
        HideOverlay(false);
        return;
      case ActionKind::CopyPath:
        CopyTextToClipboard(AppPathForActions(app));
        HideOverlay(false);
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
    if (!persistenceExecutor_.Submit(
            [this, storedText, preview, capturedAt, retention](std::stop_token stopToken) {
              if (stopToken.stop_requested()) return;
              bool saved = true;
              {
                std::lock_guard lock(storageMutex_);
                if (storage_.IsOpen()) {
                  saved = storage_.AddClipboardEntry(storedText, preview, capturedAt,
                                                     retention).has_value();
                }
              }
              if (!saved && !stopThreads_ &&
                  !persistenceErrorQueued_.exchange(true, std::memory_order_acq_rel)) {
                PostMessageW(hwnd_, WM_PERSISTENCE_ERROR, 0, 0);
              }
            }) &&
        !persistenceErrorQueued_.exchange(true, std::memory_order_acq_rel)) {
      PostMessageW(hwnd_, WM_PERSISTENCE_ERROR, 0, 0);
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
    HWND target = lastActiveWindow_;
    const UINT existingFormats = CountClipboardFormats();
    ComPtr<IDataObject> previousClipboard;
    const HRESULT captureResult = OleGetClipboard(&previousClipboard);
    if (existingFormats > 0 && FAILED(captureResult)) {
      ShowTrayNotification(L"FeatherCast Paste",
                           L"The current clipboard could not be preserved, so FeatherCast did not replace it.");
      return;
    }

    HideOverlay(false);
    if (!CopyTextToClipboard(text)) {
      ShowTrayNotification(L"FeatherCast Paste", L"FeatherCast could not place the text on the clipboard.");
      return;
    }
    const DWORD temporarySequence = GetClipboardSequenceNumber();
    lastActiveWindow_ = nullptr;

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
    {
      std::lock_guard lock(iconQueueMutex_);
      iconJobs_.clear();
      pendingIcons_.clear();
    }
    ClearIconBitmaps();
    std::error_code ec;
    std::filesystem::remove_all(IconCacheDir(), ec);
    std::filesystem::create_directories(IconCacheDir(), ec);
    if (!stopThreads_) StartIconWorkers();
    StartAppDiscovery();
    RequestSearch();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void StopIconThreads() {
    iconCv_.notify_all();
    std::lock_guard lock(workerThreadsMutex_);
    for (auto& thread : iconThreads_) {
      if (thread.joinable()) thread.request_stop();
    }
    iconCv_.notify_all();
    for (auto& thread : iconThreads_) {
      if (thread.joinable()) thread.join();
    }
    iconThreads_.clear();
  }

  void RestartApp() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring command = L"/c timeout /t 1 /nobreak >nul & start \"\" \"" + std::wstring(exePath) + L"\" --show";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", command.c_str(), nullptr, SW_HIDE);
    DestroyWindow(hwnd_);
  }

  bool LaunchApp(const AppEntry& app, bool asAdmin) {
    DebugLaunchLog(L"LaunchApp name='" + app.name + L"' type=" +
                   std::to_wstring(static_cast<int>(app.launchType)) + L" target='" +
                   app.launchTarget + L"' targetPath='" + app.targetPath + L"' admin=" +
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
        DebugLaunchLog(L"  AppsFolder admin fallback target='" + adminTarget + L"'");
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
      DebugLaunchLog(L"  fallback ShellExecute target='" + target + L"' result=" +
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
    DebugLaunchLog(L"  Shortcut/Exe file='" + file + L"' ok=" + std::to_wstring(ok) +
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

  HINSTANCE instance_ = nullptr;
  std::wstring cmdLine_;
  HWND hwnd_ = nullptr;
  HWND settingsHwnd_ = nullptr;
  NOTIFYICONDATAW tray_{};
  HHOOK hook_ = nullptr;
  bool hotKeyRegistered_ = false;
  bool clipboardListenerRegistered_ = false;
  bool hadLegacyOperationalData_ = false;
  UINT taskbarCreatedMessage_ = 0;
  ULONGLONG lastShortcutToggleTick_ = 0;
  Settings settings_;
  ShortcutSpec shortcut_;
  HWND lastActiveWindow_ = nullptr;
  bool visible_ = false;
  bool highContrast_ = false;
  bool systemAnimationsEnabled_ = true;
  bool suppressHide_ = false;
  LARGE_INTEGER qpcFrequency_{};
  LONGLONG animStartQpc_ = 0;
  LONGLONG lastAnimationFrameQpc_ = 0;
  LONGLONG lastSelectionFrameQpc_ = 0;
  bool animating_ = false;
  bool animationFrameQueued_ = false;
  float visualSelectedY_ = -1.0f;
  bool animatingSelection_ = false;
  double selectionSettleSeconds_ = 0.090;
  bool selectionTimerActive_ = false;
  View view_ = View::Search;
  HMONITOR overlayMonitor_ = nullptr;
  std::wstring query_;
  std::wstring imeComposition_;
  size_t caret_ = 0;
  std::optional<size_t> selectionAnchor_;
  int selected_ = 0;
  int scroll_ = 0;
  bool actionMode_ = false;
  DisplayItem actionTarget_;
  BrowseView browseView_ = BrowseView::None;
  std::vector<NavigationState> navigationStack_;
  std::optional<NavigationState> pendingNavigationRestore_;
  bool recording_ = false;
  bool gearHovered_ = false;
  bool mouseTracking_ = false;
  bool ignoreMouseUntilMove_ = false;
  POINT mouseAnchor_ = {0, 0};
  int settingsHover_ = -1;
  int settingsFocusIndex_ = 0;
  float settingsScroll_ = 0.0f;
  std::wstring pendingShortcut_;
  ShortcutRecorder shortcutRecorder_;
  ShortcutRuntime shortcutRuntime_;
  PressedModifiers hookModifiers_;
  std::atomic<bool> stopThreads_ = false;
  std::atomic<bool> shutdownStarted_ = false;
  std::atomic<bool> caching_ = false;
  std::atomic<bool> windowRefreshPending_ = false;
  feathercast::background::Executor launchExecutor_;
  feathercast::background::Executor persistenceExecutor_;
  std::mutex settingsPersistenceMutex_;
  std::optional<Settings> pendingSettingsSave_;
  bool settingsSaveScheduled_ = false;
  std::atomic<bool> persistenceErrorQueued_ = false;
  std::mutex completedLaunchMutex_;
  std::deque<LaunchCompletion> completedLaunches_;
  std::atomic<bool> appsReady_ = false;
  std::jthread discoveryThread_;
  std::mutex discoveryMutex_;
  std::condition_variable discoveryCv_;
  std::optional<DiscoveryRequest> pendingDiscovery_;
  std::atomic<uint64_t> latestDiscoveryGeneration_ = 0;
  std::jthread searchThread_;
  std::jthread snapshotThread_;
  std::mutex searchMutex_;
  std::condition_variable searchCv_;
  std::optional<QueryRequest> pendingRequest_;
  ResultsCollection latestResult_;
  std::mutex snapshotMutex_;
  std::condition_variable snapshotCv_;
  std::optional<SnapshotBuildRequest> pendingSnapshotBuild_;
  std::optional<SnapshotBuildResult> latestSnapshotBuild_;
  uint64_t snapshotScheduledRevision_ = 0;
  unsigned long long searchGeneration_ = 0;
  std::atomic<unsigned long long> latestRequestedSearchGeneration_ = 0;
  // Cached query-independent search corpus, rebuilt when the published data
  // revision changes. Revisions cannot lose a concurrent invalidation.
  std::shared_ptr<const SearchSnapshot> snapshot_;
  std::atomic<uint64_t> dataRevision_ = 1;
  uint64_t snapshotRevision_ = 0;
  std::mutex workerThreadsMutex_;
  std::vector<std::jthread> iconThreads_;
  std::mutex dataMutex_;
  std::mutex storageMutex_;
  feathercast::storage::Storage storage_;
  feathercast::theme::Theme theme_;
  std::vector<AppEntry> apps_;
  std::vector<WindowEntry> windows_;
  std::vector<AppEntry> fileIndex_;
  std::vector<AppEntry> systemFolders_;
  std::vector<feathercast::snippets::Snippet> snippets_;
  std::vector<ClipboardEntry> clipboardHistory_;
  unsigned long long clipboardSerial_ = 0;
  std::optional<std::wstring> internalClipboardText_;
  CurrencyRates currencyRates_;
  std::wstring localeCurrency_ = DetectLocaleCurrency();
  std::jthread currencyThread_;
  std::jthread updateThread_;
  std::atomic<bool> updateWorkerRunning_ = false;
  std::mutex updateMutex_;
  std::optional<UpdateTaskResult> latestUpdateResult_;
  feathercast::extensions::ExtensionManager extensions_;
  std::mutex extensionActivationMutex_;
  std::optional<ExtensionActivationResult> latestExtensionActivation_;
  bool extensionActivationPending_ = false;
  std::vector<Section> sections_;
  std::vector<DisplayItem> flatItems_;
  std::vector<HitTarget> hits_;
  std::mutex iconQueueMutex_;
  std::condition_variable iconCv_;
  std::atomic<bool> iconReadyMessageQueued_ = false;
  std::set<std::wstring> pendingIcons_;
  std::deque<std::wstring> iconJobs_;
  // LRU-bounded in-memory bitmap cache. iconLru_ holds keys most-recent-first;
  // each map entry stores its position so it can be promoted/evicted in O(1).
  struct IconCacheEntry {
    ComPtr<ID2D1Bitmap> bitmap;
    std::list<std::wstring>::iterator lruIt;
  };
  std::list<std::wstring> iconLru_;
  std::unordered_map<std::wstring, IconCacheEntry> iconBitmaps_;
  static constexpr size_t kIconCacheCap = 256;

  ComPtr<IDWriteFactory> dwriteFactory_;
  ComPtr<IWICImagingFactory> wicFactory_;

  // Shared Direct3D 11 / Direct2D / DirectComposition device stack (see EnsureGlassDevice).
  ComPtr<ID3D11Device> d3dDevice_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<IDXGIFactory2> dxgiFactory_;
  ComPtr<IDCompositionDevice> dcompDevice_;

  GlassSurface overlaySurface_;
  GlassSurface settingsSurface_;
  std::wstring caretMeasureText_ = L"\x01";  // sentinel that never equals a real measured prefix
  float caretMeasureWidth_ = -1.0f;
  float caretOffset_ = 0.0f;
  bool lastCaretPhase_ = false;
  ID2D1RenderTarget* activeRT_ = nullptr;
  // Per-render-target cache of solid-color brushes, keyed by packed RGBA.
  std::unordered_map<ID2D1RenderTarget*, std::unordered_map<uint32_t, ComPtr<ID2D1SolidColorBrush>>> brushCache_;
  ComPtr<ID2D1DeviceContext> activeDC_;  // QI of activeRT_ for color-emoji DrawText, when available
  ComPtr<IDWriteTextFormat> inputFormat_;
  ComPtr<IDWriteTextFormat> rowFormat_;
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
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
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

  std::wstring cmdLineStr = cmdLine ? cmdLine : L"";
  FeatherCastApp app(instance, cmdLineStr);
  return app.Run();
}
