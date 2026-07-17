#include "result_icons.hpp"

namespace feathercast::ui {
namespace {

ResultIcon CapabilityIcon(const std::wstring& id) noexcept {
  if (id == L"apps") return ResultIcon::AppGrid;
  if (id == L"windows") return ResultIcon::Windows;
  if (id == L"window-management") return ResultIcon::WindowLayout;
  if (id == L"actions") return ResultIcon::Actions;
  if (id == L"calculator") return ResultIcon::Calculator;
  if (id == L"conversions") return ResultIcon::Convert;
  if (id == L"time-utilities") return ResultIcon::Clock;
  if (id == L"uuid") return ResultIcon::Code;
  if (id == L"web-search") return ResultIcon::WebSearch;
  if (id == L"emoji") return ResultIcon::Smile;
  if (id == L"symbols") return ResultIcon::Symbols;
  if (id == L"files") return ResultIcon::FolderSearch;
  if (id == L"clipboard") return ResultIcon::Clipboard;
  if (id == L"snippets") return ResultIcon::Document;
  if (id == L"quicklinks") return ResultIcon::Link;
  if (id == L"system-commands") return ResultIcon::Terminal;
  if (id == L"windows-settings") return ResultIcon::Gear;
  if (id == L"plugins") return ResultIcon::Puzzle;
  if (id == L"shortcut") return ResultIcon::Keyboard;
  return ResultIcon::App;
}

ResultIcon CommandIcon(app::CommandKind kind) noexcept {
  using app::CommandKind;
  switch (kind) {
    case CommandKind::Settings: return ResultIcon::Gear;
    case CommandKind::Quit: return ResultIcon::Exit;
    case CommandKind::Restart:
    case CommandKind::RefreshApps: return ResultIcon::Refresh;
    case CommandKind::ClearIconCache:
    case CommandKind::EmptyRecycleBin: return ResultIcon::Trash;
    case CommandKind::ClearRecents: return ResultIcon::HistoryOff;
    case CommandKind::OpenDataFolder: return ResultIcon::FolderGear;
    case CommandKind::OpenLocalDataFolder: return ResultIcon::Database;
    case CommandKind::ReloadExtensions: return ResultIcon::PuzzleRefresh;
    case CommandKind::LockPC: return ResultIcon::Lock;
    case CommandKind::SleepPC: return ResultIcon::Moon;
    case CommandKind::MuteAudio: return ResultIcon::SpeakerOff;
    case CommandKind::ShutDown: return ResultIcon::Power;
    case CommandKind::RestartPC: return ResultIcon::PowerRefresh;
    case CommandKind::ClearClipboardHistory: return ResultIcon::ClipboardOff;
    case CommandKind::OpenSnippetsFile: return ResultIcon::Document;
    case CommandKind::ReloadSnippets: return ResultIcon::DocumentRefresh;
    case CommandKind::OpenThemeFile: return ResultIcon::Palette;
    case CommandKind::ReloadTheme: return ResultIcon::PaletteRefresh;
    case CommandKind::CheckForUpdates: return ResultIcon::Download;
    case CommandKind::ClipboardHistory: return ResultIcon::Clipboard;
    case CommandKind::EmojiPicker: return ResultIcon::Smile;
    case CommandKind::DiscoverFeatherCast: return ResultIcon::Compass;
    case CommandKind::VolumeControl: return ResultIcon::Sliders;
    case CommandKind::VolumeUp: return ResultIcon::SpeakerPlus;
    case CommandKind::VolumeDown: return ResultIcon::SpeakerMinus;
    case CommandKind::MediaPlayPause: return ResultIcon::PlayPause;
    case CommandKind::MediaNext: return ResultIcon::NextTrack;
    case CommandKind::MediaPrevious: return ResultIcon::PreviousTrack;
    case CommandKind::ShowDesktop: return ResultIcon::Monitor;
    case CommandKind::GenerateUuid: return ResultIcon::Code;
  }
  return ResultIcon::App;
}

ResultIcon ActionIcon(app::ActionKind kind) noexcept {
  using app::ActionKind;
  switch (kind) {
    case ActionKind::Open: return ResultIcon::ExternalLink;
    case ActionKind::RunAsAdmin: return ResultIcon::Shield;
    case ActionKind::OpenLocation: return ResultIcon::Folder;
    case ActionKind::CopyPath:
    case ActionKind::CopyText: return ResultIcon::Copy;
    case ActionKind::Pin: return ResultIcon::Pin;
    case ActionKind::Unpin: return ResultIcon::PinOff;
    case ActionKind::Hide: return ResultIcon::EyeOff;
    case ActionKind::Unhide: return ResultIcon::Eye;
    case ActionKind::Switch: return ResultIcon::Windows;
    case ActionKind::Minimize: return ResultIcon::Minimize;
    case ActionKind::MaximizeRestore: return ResultIcon::Maximize;
    case ActionKind::CloseWindow: return ResultIcon::Close;
    case ActionKind::MoveWindowLeftHalf: return ResultIcon::WindowLeft;
    case ActionKind::MoveWindowRightHalf: return ResultIcon::WindowRight;
    case ActionKind::MoveWindowTopHalf: return ResultIcon::WindowTop;
    case ActionKind::MoveWindowBottomHalf: return ResultIcon::WindowBottom;
    case ActionKind::CenterWindow: return ResultIcon::Center;
    case ActionKind::MoveWindowNextDisplay: return ResultIcon::MultiMonitor;
    case ActionKind::PasteText: return ResultIcon::Clipboard;
    case ActionKind::None: return ResultIcon::Actions;
  }
  return ResultIcon::Actions;
}

}  // namespace

ResultIcon ResolveResultIcon(const app::DisplayItem& item) noexcept {
  if (item.isCapability) return CapabilityIcon(item.capability.stableId);
  if (item.isCommand) return CommandIcon(item.command);
  if (item.isAction) return ActionIcon(item.action);
  if (item.isCalculator) return ResultIcon::Calculator;
  if (item.isConversion) return ResultIcon::Convert;
  if (item.isWebSearch) return ResultIcon::WebSearch;
  if (item.isExtension) return ResultIcon::Puzzle;
  if (item.isSnippet) return ResultIcon::Document;
  if (item.isClipboard) return ResultIcon::Clipboard;
  if (item.isRunCommand) return ResultIcon::Terminal;
  if (item.isSymbol) return ResultIcon::Symbols;
  if (item.utility) {
    switch (item.utility->kind) {
      case app::UtilityKind::LocalTime: return ResultIcon::Clock;
      case app::UtilityKind::LocalDate: return ResultIcon::Calendar;
      case app::UtilityKind::IsoWeek: return ResultIcon::CalendarWeek;
      case app::UtilityKind::UnixTime: return ResultIcon::Code;
    }
  }
  if (item.isWindow) return ResultIcon::Windows;
  if (item.app.source == L"quicklink") return ResultIcon::Link;
  if (item.app.source == L"file") {
    return item.app.fileIsDirectory ? ResultIcon::Folder : ResultIcon::File;
  }
  if (item.app.source == L"system-folder") return ResultIcon::Folder;
  if (item.app.source == L"windows-settings") return ResultIcon::Gear;
  return ResultIcon::App;
}

}  // namespace feathercast::ui
