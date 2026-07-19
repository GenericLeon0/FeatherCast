#include "settings_catalog.hpp"

#include <set>

namespace feathercast::settings_catalog {

const std::vector<CategoryDescriptor>& Categories() {
  using app::HitType;
  using app::SettingsCategory;
  static const std::vector<CategoryDescriptor> categories = {
      {SettingsCategory::Shortcut, HitType::SettingsShortcutCategory,
       L"Shortcut", L"Shortcut settings"},
      {SettingsCategory::General, HitType::SettingsGeneralCategory,
       L"General", L"General settings"},
      {SettingsCategory::Results, HitType::SettingsResultsCategory,
       L"Results", L"Results settings"},
      {SettingsCategory::Library, HitType::SettingsLibraryCategory,
       L"Library", L"Snippet and quicklink library"},
      {SettingsCategory::Privacy, HitType::SettingsPrivacyCategory,
       L"Privacy", L"Privacy settings"},
      {SettingsCategory::Extensions, HitType::SettingsExtensionsCategory,
       L"Extensions", L"Extensions settings"},
      {SettingsCategory::Appearance, HitType::SettingsAppearanceCategory,
       L"Appearance", L"Appearance settings"},
      {SettingsCategory::Maintenance, HitType::SettingsMaintenanceCategory,
       L"Maintenance", L"Maintenance settings"},
  };
  return categories;
}

const std::vector<SettingDescriptor>& Catalog() {
  using app::HitType;
  using app::SettingsCategory;
  static const std::vector<SettingDescriptor> controls = {
      {L"shortcut.record", SettingsCategory::Shortcut, HitType::RecordShortcut,
       ControlKind::Custom, L"Record shortcut", L"Capture a new global shortcut.",
       L"Record global shortcut"},
      {L"shortcut.save", SettingsCategory::Shortcut, HitType::SaveShortcut,
       ControlKind::Action, L"Save shortcut", L"Activate the recorded shortcut.",
       L"Save global shortcut", Requirement::PendingShortcut},
      {L"shortcut.clear", SettingsCategory::Shortcut, HitType::ClearShortcut,
       ControlKind::Action, L"Clear shortcut", L"Disable the global shortcut.",
       L"Clear global shortcut", Requirement::ExistingShortcut},
      {L"general.startup", SettingsCategory::General, HitType::StartupToggle,
       ControlKind::Toggle, L"Start on Startup",
       L"Launch FeatherCast when you log into Windows.", L"Start on startup"},
      {L"general.updates", SettingsCategory::General, HitType::UpdateChecksToggle,
       ControlKind::Toggle, L"Automatic Update Checks",
       L"Check GitHub Releases once per day.", L"Automatic update checks"},
      {L"general.compact", SettingsCategory::General, HitType::CompactToggle,
       ControlKind::Toggle, L"Compact Mode",
       L"Show only the search bar at rest; results expand below.", L"Compact mode"},
      {L"general.animations", SettingsCategory::General, HitType::AnimationLevel,
       ControlKind::Slider, L"Animation",
       L"Choose how much interface motion FeatherCast uses.", L"Animation level"},
      {L"results.windows", SettingsCategory::Results, HitType::ShowWindowsToggle,
       ControlKind::Toggle, L"Open Window Results",
       L"Include currently open windows in search results.", L"Open window results"},
      {L"results.store-apps", SettingsCategory::Results, HitType::ShowStoreAppsToggle,
       ControlKind::Toggle, L"Store/System Apps",
       L"Include AppsFolder, Store, and system alias entries.", L"Store and system apps"},
      {L"results.width.down", SettingsCategory::Results, HitType::OverlayWidthDown,
       ControlKind::Decrement, L"Overlay Width", L"Width of the search overlay window.",
       L"Decrease overlay width"},
      {L"results.width.up", SettingsCategory::Results, HitType::OverlayWidthUp,
       ControlKind::Increment, L"Overlay Width", L"Width of the search overlay window.",
       L"Increase overlay width"},
      {L"results.maximum.down", SettingsCategory::Results, HitType::MaxResultsDown,
       ControlKind::Decrement, L"Max Results", L"Maximum number of results to show.",
       L"Decrease maximum results"},
      {L"results.maximum.up", SettingsCategory::Results, HitType::MaxResultsUp,
       ControlKind::Increment, L"Max Results", L"Maximum number of results to show.",
       L"Increase maximum results"},
      {L"library.snippets", SettingsCategory::Library, HitType::ManageSnippets,
       ControlKind::Action, L"Manage Snippets",
       L"Create, edit, and delete reusable text snippets.",
       L"Manage snippets"},
      {L"library.quicklinks", SettingsCategory::Library, HitType::ManageQuicklinks,
       ControlKind::Action, L"Manage Quicklinks",
       L"Create, edit, and delete keyword shortcuts for URLs, files, and folders.",
       L"Manage quicklinks"},
      {L"privacy.clipboard", SettingsCategory::Privacy, HitType::ClipboardHistoryToggle,
       ControlKind::Toggle, L"Clipboard History",
       L"Store copied text locally for launcher search and paste.", L"Clipboard history"},
      {L"privacy.clipboard-limit.down", SettingsCategory::Privacy, HitType::ClipboardLimitDown,
       ControlKind::Decrement, L"Clipboard Retention",
       L"Maximum number of text entries retained locally.",
       L"Decrease clipboard history retention", Requirement::ClipboardEnabled},
      {L"privacy.clipboard-limit.up", SettingsCategory::Privacy, HitType::ClipboardLimitUp,
       ControlKind::Increment, L"Clipboard Retention",
       L"Maximum number of text entries retained locally.",
       L"Increase clipboard history retention", Requirement::ClipboardEnabled},
      {L"privacy.file-index", SettingsCategory::Privacy, HitType::FileIndexToggle,
       ControlKind::Toggle, L"Files & Folders Index",
       L"Index selected local folders for launcher search.", L"Files and folders index"},
      {L"privacy.file-content", SettingsCategory::Privacy,
       HitType::FileContentIndexToggle, ControlKind::Toggle,
       L"Search File Contents",
       L"Build a local searchable token index for supported text files.",
       L"Search file contents", Requirement::FileIndexEnabled},
      {L"privacy.file-limit.down", SettingsCategory::Privacy, HitType::FileIndexLimitDown,
       ControlKind::Decrement, L"File Index Limit",
       L"Maximum number of files and folders stored locally.",
       L"Decrease file index limit", Requirement::FileIndexEnabled},
      {L"privacy.file-limit.up", SettingsCategory::Privacy, HitType::FileIndexLimitUp,
       ControlKind::Increment, L"File Index Limit",
       L"Maximum number of files and folders stored locally.",
       L"Increase file index limit", Requirement::FileIndexEnabled},
      {L"privacy.add-root", SettingsCategory::Privacy, HitType::AddFileRoot,
       ControlKind::Action, L"Add Indexed Folder", L"Add a folder to the local index.",
       L"Add file index folder", Requirement::FileIndexEnabled},
      {L"privacy.remove-root", SettingsCategory::Privacy, HitType::RemoveFileRoot,
       ControlKind::Action, L"Remove Indexed Folder",
       L"Remove one configured folder from the local index.",
       L"Remove indexed folder", Requirement::FileIndexEnabled},
      {L"privacy.default-roots", SettingsCategory::Privacy, HitType::ClearFileRoots,
       ControlKind::Action, L"Use Default Folders",
       L"Index Desktop, Documents, and Downloads.", L"Use default file index folders",
       Requirement::FileIndexEnabled},
      {L"privacy.rebuild-files", SettingsCategory::Privacy,
       HitType::RebuildFileIndex, ControlKind::Action, L"Rebuild File Index",
       L"Reconcile selected folders and rebuild searchable content.",
       L"Rebuild file index", Requirement::FileIndexEnabled},
      {L"privacy.diagnostics", SettingsCategory::Privacy, HitType::DiagnosticsToggle,
       ControlKind::Toggle, L"Diagnostics",
       L"Write bounded troubleshooting logs without queries or clipboard text.",
       L"Enable diagnostics"},
      {L"privacy.clear-clipboard", SettingsCategory::Privacy, HitType::ClearClipboardData,
       ControlKind::Action, L"Delete Clipboard Data", L"Delete saved clipboard entries.",
       L"Delete clipboard data", Requirement::StorageIdle},
      {L"privacy.clear-files", SettingsCategory::Privacy, HitType::ClearFileIndexData,
       ControlKind::Action, L"Delete File Index", L"Delete the disposable local file index.",
       L"Delete file index", Requirement::StorageIdle},
      {L"privacy.open-data", SettingsCategory::Privacy, HitType::OpenLocalDataFolder,
       ControlKind::Action, L"Open Local Data", L"Open logs, cache, database, and updates.",
       L"Open local data folder"},
      {L"extensions.reload", SettingsCategory::Extensions, HitType::ReloadExtensions,
       ControlKind::Action, L"Reload Extensions", L"Restart and reload installed plugins.",
       L"Reload extensions", Requirement::ExtensionsIdle},
      {L"extensions.open-folder", SettingsCategory::Extensions, HitType::OpenPluginsFolder,
       ControlKind::Action, L"Open Plugins Folder", L"Open the trusted native plugin folder.",
       L"Open plugins folder"},
      {L"appearance.system-accent", SettingsCategory::Appearance, HitType::AccentToggle,
       ControlKind::Toggle, L"Sync Accent Color",
       L"Match the Windows accent color automatically.", L"Sync accent color"},
      {L"appearance.custom-accent", SettingsCategory::Appearance, HitType::AccentColor,
       ControlKind::Custom, L"Accent Color", L"Choose a custom launcher accent color.",
       L"Pick accent color", Requirement::CustomAccent},
      {L"maintenance.clear-recents", SettingsCategory::Maintenance, HitType::ClearRecents,
       ControlKind::Action, L"Clear Recents", L"Forget recent apps and usage ranking.",
       L"Clear recents"},
      {L"maintenance.clear-icons", SettingsCategory::Maintenance, HitType::ClearIconCache,
       ControlKind::Action, L"Clear Icon Cache", L"Delete cached Windows Shell icons.",
       L"Clear icon cache"},
      {L"maintenance.check-updates", SettingsCategory::Maintenance, HitType::CheckUpdates,
       ControlKind::Action, L"Check Updates", L"Check GitHub Releases now.",
       L"Check for updates"},
  };
  return controls;
}

const SettingDescriptor* Find(app::HitType hit) {
  for (const auto& descriptor : Catalog()) {
    if (descriptor.hit == hit) return &descriptor;
  }
  return nullptr;
}

const CategoryDescriptor* FindCategory(app::SettingsCategory category) {
  for (const auto& descriptor : Categories()) {
    if (descriptor.category == category) return &descriptor;
  }
  return nullptr;
}

const CategoryDescriptor* FindCategory(app::HitType hit) {
  for (const auto& descriptor : Categories()) {
    if (descriptor.hit == hit) return &descriptor;
  }
  return nullptr;
}

bool Enabled(const SettingDescriptor& descriptor,
             const CatalogContext& context) {
  switch (descriptor.requirement) {
    case Requirement::Always: return true;
    case Requirement::PendingShortcut: return context.hasPendingShortcut;
    case Requirement::ExistingShortcut:
      return !context.hasPendingShortcut && context.hasExistingShortcut;
    case Requirement::ClipboardEnabled:
      return context.clipboardEnabled && context.storageIdle;
    case Requirement::FileIndexEnabled:
      return context.fileIndexEnabled && context.storageIdle;
    case Requirement::StorageIdle: return context.storageIdle;
    case Requirement::ExtensionsIdle: return context.extensionsIdle;
    case Requirement::CustomAccent: return context.customAccent;
  }
  return false;
}

bool Checked(app::HitType hit, const app::Settings& settings) {
  using app::HitType;
  switch (hit) {
    case HitType::StartupToggle: return settings.startOnStartup;
    case HitType::UpdateChecksToggle: return settings.updateChecksEnabled;
    case HitType::CompactToggle: return settings.compactMode;
    case HitType::ShowWindowsToggle: return settings.showOpenWindows;
    case HitType::ShowStoreAppsToggle: return settings.showStoreApps;
    case HitType::ClipboardHistoryToggle: return settings.clipboardHistoryEnabled;
    case HitType::FileIndexToggle: return settings.fileIndexEnabled;
    case HitType::FileContentIndexToggle:
      return settings.fileContentIndexEnabled;
    case HitType::DiagnosticsToggle: return settings.diagnosticsEnabled;
    case HitType::AccentToggle: return settings.syncAccentColor;
    default: return false;
  }
}

std::vector<app::HitType> FocusOrder(app::SettingsCategory category,
                                     const CatalogContext& context) {
  std::vector<app::HitType> order;
  for (const auto& control : Catalog()) {
    if (control.category == category && Enabled(control, context)) {
      order.push_back(control.hit);
    }
  }
  return order;
}

bool ValidateCatalog(std::wstring* error) {
  std::set<std::wstring_view> ids;
  std::set<app::HitType> hits;
  for (const auto& descriptor : Catalog()) {
    if (descriptor.stableId.empty() || descriptor.label.empty() ||
        descriptor.description.empty() || descriptor.accessibleName.empty() ||
        !ids.insert(descriptor.stableId).second ||
        !hits.insert(descriptor.hit).second) {
      if (error) *error = L"Setting metadata and stable identifiers must be complete and unique.";
      return false;
    }
  }
  return Categories().size() == 8;
}

}  // namespace feathercast::settings_catalog
