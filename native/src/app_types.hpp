#pragma once

#include "core.hpp"
#include "clock_utilities.hpp"
#include "extension_protocol.hpp"
#include "run_command.hpp"
#include "settings.hpp"
#include "snippets.hpp"
#include "symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace feathercast::app {

enum class View {
  Search,
  Settings,
};

enum class SettingsCategory {
  Shortcut,
  General,
  Results,
  Privacy,
  Extensions,
  Appearance,
  Maintenance,
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
  ConfirmCancel,
  ConfirmAction,
  Back,
  CloseSettings,
  SettingsShortcutCategory,
  SettingsGeneralCategory,
  SettingsResultsCategory,
  SettingsPrivacyCategory,
  SettingsExtensionsCategory,
  SettingsAppearanceCategory,
  SettingsMaintenanceCategory,
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
  DiscoverFeatherCast,
  VolumeControl,
  VolumeUp,
  VolumeDown,
  MediaPlayPause,
  MediaNext,
  MediaPrevious,
  ShowDesktop,
  GenerateUuid,
};

struct ConfirmationDialog {
  CommandKind command = CommandKind::ShutDown;
  std::wstring title;
  std::wstring message;
  std::wstring actionLabel;
};

enum class StatusSeverity {
  Info,
  Progress,
  Success,
  Error,
};

struct StatusMessage {
  StatusSeverity severity = StatusSeverity::Info;
  std::wstring text;
};

enum class StorageOperationKind {
  ClearClipboard,
  ClearFileIndex,
};

struct StorageOperationResult {
  StorageOperationKind kind = StorageOperationKind::ClearClipboard;
  bool succeeded = false;
  std::wstring error;
};

enum class BrowseView {
  None,
  Clipboard,
  Emoji,
  Capabilities,
};

enum class CapabilityActionKind {
  SeedQuery,
  OpenBrowse,
  OpenSettings,
  RunCommand,
};

struct CapabilityAction {
  CapabilityActionKind kind = CapabilityActionKind::SeedQuery;
  std::wstring query;
  BrowseView browseView = BrowseView::None;
  SettingsCategory settingsCategory = SettingsCategory::General;
  CommandKind command = CommandKind::Settings;
};

struct CapabilityItem {
  std::wstring stableId;
  std::wstring category;
  std::wstring title;
  std::wstring summary;
  std::wstring example;
  CapabilityAction action;
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
  MoveWindowLeftHalf,
  MoveWindowRightHalf,
  MoveWindowTopHalf,
  MoveWindowBottomHalf,
  CenterWindow,
  MoveWindowNextDisplay,
  CopyText,
  PasteText,
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

struct PointerPress {
  HWND owner = nullptr;
  HitType type = HitType::Result;
  int index = -1;
  unsigned long long searchGeneration = 0;
  std::wstring targetKey;
  bool inside = false;
};

using Settings = feathercast::settings::Settings;

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

struct CurrencyRates {
  std::map<std::wstring, double> perUsd;
  long long fetchedAt = 0;
};

enum class UtilityKind {
  LocalTime,
  LocalDate,
  IsoWeek,
  UnixTime,
};

struct UtilityResult {
  UtilityKind kind = UtilityKind::LocalTime;
  std::wstring stableId;
  std::wstring title;
  std::wstring value;
  std::vector<std::wstring> keywords;
};

struct TextActionPayload {
  std::wstring value;
};

using ActionTarget =
    std::variant<std::monostate, AppEntry, WindowEntry, TextActionPayload>;

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
  bool isCapability = false;
  std::optional<UtilityResult> utility;
  AppEntry app;
  WindowEntry window;
  feathercast::extensions::QueryResultItem extension;
  feathercast::snippets::Snippet snippet;
  ClipboardEntry clipboard;
  feathercast::run_command::Command runCommand;
  feathercast::symbols::Symbol symbol;
  CapabilityItem capability;
  CommandKind command = CommandKind::Settings;
  ActionKind action = ActionKind::None;
  ActionTarget actionTarget;
  std::wstring commandName;
  std::wstring commandDetail;
  std::vector<std::wstring> commandKeywords;
  std::wstring calculationExpression;
  std::wstring calculationResult;
  std::wstring webSearchUrl;
  std::wstring webSearchLabel;

  std::wstring Key() const {
    if (isCapability) return L"capability:" + capability.stableId;
    if (isCalculator) return L"calc:" + calculationExpression;
    if (isConversion) return L"conv:" + calculationExpression;
    if (isWebSearch) return L"web:" + webSearchUrl;
    if (isExtension) return L"ext:" + extension.pluginId + L":" + extension.id;
    if (isSnippet) return L"snippet:" + snippet.keyword;
    if (isClipboard) return L"clip:" + clipboard.id;
    if (isRunCommand) {
      return L"run:" + std::to_wstring(static_cast<int>(runCommand.kind)) +
             L":" + runCommand.target;
    }
    if (isSymbol) return L"symbol:" + symbol.value;
    if (utility) return L"utility:" + utility->stableId;
    if (isCommand) {
      return L"cmd:" + std::to_wstring(static_cast<int>(command));
    }
    if (isAction) {
      std::wstring target;
      if (const auto* windowTarget = std::get_if<WindowEntry>(&actionTarget)) {
        target = std::to_wstring(
            reinterpret_cast<std::uintptr_t>(windowTarget->hwnd));
      } else if (const auto* appTarget = std::get_if<AppEntry>(&actionTarget)) {
        target = !appTarget->id.empty() ? appTarget->id : appTarget->path;
      } else if (const auto* textTarget =
                     std::get_if<TextActionPayload>(&actionTarget)) {
        target = textTarget->value;
      }
      return L"act:" + std::to_wstring(static_cast<int>(action)) + L":" +
             target;
    }
    if (isWindow) {
      return L"win:" +
             std::to_wstring(reinterpret_cast<std::uintptr_t>(window.hwnd));
    }
    return !app.id.empty() ? app.id : app.path;
  }

  std::wstring Name() const {
    if (isCapability) return capability.title;
    if (isCalculator || isConversion) return calculationResult;
    if (isWebSearch) return webSearchLabel;
    if (isExtension) return extension.title;
    if (isSnippet) return snippet.name;
    if (isClipboard) return clipboard.preview;
    if (isRunCommand) return runCommand.label;
    if (isSymbol) return symbol.label;
    if (utility) return utility->title;
    if (isCommand || isAction) return commandName;
    return isWindow ? window.name : app.name;
  }

  std::wstring IconKey() const {
    if (isCapability) return L"";
    if (isCalculator || isConversion || isWebSearch || isRunCommand ||
        isSymbol || utility) {
      return L"";
    }
    if (isExtension) return extension.iconPath;
    if (isSnippet || isClipboard) return L"";
    if (isAction) {
      if (const auto* windowTarget = std::get_if<WindowEntry>(&actionTarget)) {
        return !windowTarget->iconKey.empty() ? windowTarget->iconKey
                                              : windowTarget->exe;
      }
      if (const auto* appTarget = std::get_if<AppEntry>(&actionTarget)) {
        return !appTarget->iconKey.empty() ? appTarget->iconKey
                                           : appTarget->path;
      }
      return L"";
    }
    if (isCommand) return L"";
    return isWindow
               ? (!window.iconKey.empty() ? window.iconKey : window.exe)
               : (!app.iconKey.empty() ? app.iconKey : app.path);
  }
};

struct Section {
  std::wstring title;
  std::vector<DisplayItem> items;
};

struct SearchSnapshot {
  std::vector<DisplayItem> pool;
  std::vector<feathercast::core::PreparedSearchItem> searchItems;
  std::vector<DisplayItem> pinned;
  std::vector<DisplayItem> recent;
  std::vector<DisplayItem> windowItems;
  std::vector<DisplayItem> system;
  std::vector<DisplayItem> systemFolders;
  std::vector<DisplayItem> commandItems;
  std::vector<DisplayItem> snippetItems;
  std::vector<DisplayItem> clipboardItems;
  std::vector<feathercast::core::SearchItem> clipboardSearchItems;
};

struct SnapshotBuildRequest {
  std::uint64_t revision = 0;
  Settings settings;
};

struct SnapshotBuildResult {
  std::uint64_t revision = 0;
  std::shared_ptr<const SearchSnapshot> snapshot;
};

struct DiscoveryRequest {
  std::uint64_t generation = 0;
  bool fileIndexEnabled = false;
  std::size_t fileIndexLimit = 0;
  std::vector<std::wstring> configuredRoots;
};

struct DiscoveryResult {
  std::uint64_t generation = 0;
  std::vector<AppEntry> apps;
  std::vector<AppEntry> fileIndex;
  bool fileIndexBuilt = false;
};

struct QueryRequest {
  unsigned long long generation = 0;
  std::wstring query;
  bool empty = false;
  bool actionMode = false;
  BrowseView browseView = BrowseView::None;
  bool compactClear = false;
  int limit = 0;
  long long now = 0;
  feathercast::clock_utilities::ClockSnapshot clock;
  const std::atomic<unsigned long long>* latestGeneration = nullptr;
  std::set<std::wstring> recentIds;
  std::map<std::wstring, std::wstring> searchEngines;
  std::map<std::wstring, double> currencyRates;
  std::wstring defaultCurrency;
  std::shared_ptr<const SearchSnapshot> snapshot;
  std::vector<DisplayItem> extensionItems;
  std::vector<DisplayItem> actions;
  std::vector<feathercast::core::SearchItem> actionSearchItems;
};

struct ResultsCollection {
  unsigned long long generation = 0;
  std::vector<Section> sections;
  std::vector<DisplayItem> flatItems;
};

}  // namespace feathercast::app
