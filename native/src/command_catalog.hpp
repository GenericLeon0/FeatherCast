#pragma once

#include "app_types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::commands {

struct ConfirmationDescriptor {
  std::wstring title;
  std::wstring message;
  std::wstring actionLabel;
};

struct CommandDescriptor {
  std::wstring stableId;
  app::CommandKind kind = app::CommandKind::Settings;
  std::wstring label;
  std::wstring detail;
  std::vector<std::wstring> keywords;
  std::optional<ConfirmationDescriptor> confirmation;
};

struct ActionDescriptor {
  app::ActionKind kind = app::ActionKind::None;
  std::wstring label;
  std::wstring detail;
};

const std::vector<CommandDescriptor>& Catalog();
const CommandDescriptor* Find(app::CommandKind kind);
std::vector<app::DisplayItem> BuildCommandItems();
std::vector<app::DisplayItem> BuildActions(
    const app::DisplayItem& target, const app::Settings& settings);
bool ValidateCatalog(std::wstring* error = nullptr);

}  // namespace feathercast::commands
