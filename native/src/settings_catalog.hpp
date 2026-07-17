#pragma once

#include "app_types.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace feathercast::settings_catalog {

enum class ControlKind { Toggle, Decrement, Increment, Action, Custom };
enum class Requirement {
  Always,
  PendingShortcut,
  ExistingShortcut,
  ClipboardEnabled,
  FileIndexEnabled,
  StorageIdle,
  ExtensionsIdle,
  CustomAccent,
};

struct SettingDescriptor {
  std::wstring_view stableId;
  app::SettingsCategory category = app::SettingsCategory::General;
  app::HitType hit = app::HitType::CompactToggle;
  ControlKind kind = ControlKind::Toggle;
  std::wstring_view label;
  std::wstring_view description;
  std::wstring_view accessibleName;
  Requirement requirement = Requirement::Always;
};

struct CategoryDescriptor {
  app::SettingsCategory category = app::SettingsCategory::General;
  app::HitType hit = app::HitType::SettingsGeneralCategory;
  std::wstring_view label;
  std::wstring_view accessibleName;
};

struct CatalogContext {
  bool hasPendingShortcut = false;
  bool hasExistingShortcut = false;
  bool clipboardEnabled = false;
  bool fileIndexEnabled = false;
  bool storageIdle = true;
  bool extensionsIdle = true;
  bool customAccent = false;
};

const std::vector<SettingDescriptor>& Catalog();
const std::vector<CategoryDescriptor>& Categories();
const SettingDescriptor* Find(app::HitType hit);
const CategoryDescriptor* FindCategory(app::SettingsCategory category);
const CategoryDescriptor* FindCategory(app::HitType hit);
bool Enabled(const SettingDescriptor& descriptor, const CatalogContext& context);
bool Checked(app::HitType hit, const app::Settings& settings);
std::vector<app::HitType> FocusOrder(app::SettingsCategory category,
                                     const CatalogContext& context);
bool ValidateCatalog(std::wstring* error = nullptr);

}  // namespace feathercast::settings_catalog
