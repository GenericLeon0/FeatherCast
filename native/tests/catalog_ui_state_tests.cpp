#include "capability_catalog.hpp"
#include "command_catalog.hpp"
#include "settings_catalog.hpp"
#include "test_framework.hpp"
#include "ui_state.hpp"
#include "ui_renderer.hpp"

#include <algorithm>

int main() {
  feathercast::ui::HitRegions geometry;
  geometry.push_back(
      {{0, 0, 100, 100}, feathercast::app::HitType::Gear});
  geometry.push_back(
      {{20, 20, 40, 40}, feathercast::app::HitType::ConfirmAction});
  assert(geometry.At(30, 30));
  assert(geometry.At(30, 30)->type ==
         feathercast::app::HitType::ConfirmAction);
  assert(geometry.At(90, 90)->type == feathercast::app::HitType::Gear);
  assert(geometry.At(120, 120) == nullptr);

  std::wstring error;
  assert(feathercast::commands::ValidateCatalog(&error));
  assert(feathercast::capabilities::ValidateCatalog(&error));
  assert(feathercast::settings_catalog::ValidateCatalog(&error));
  const auto commands = feathercast::commands::BuildCommandItems();
  assert(!commands.empty());
  assert(std::all_of(commands.begin(), commands.end(),
                     [](const auto& item) { return item.isCommand; }));
  const auto* shutdown =
      feathercast::commands::Find(feathercast::app::CommandKind::ShutDown);
  assert(shutdown && shutdown->confirmation);
  const auto* discover = feathercast::commands::Find(
      feathercast::app::CommandKind::DiscoverFeatherCast);
  assert(discover && discover->stableId == L"discover-feathercast");
  for (const std::wstring keyword : {L"help", L"features", L"shortcuts",
                                     L"how to"}) {
    assert(std::find(discover->keywords.begin(), discover->keywords.end(),
                     keyword) != discover->keywords.end());
  }

  const auto capabilityMatches =
      feathercast::capabilities::Search(L"math expression");
  assert(!capabilityMatches.empty());
  assert(capabilityMatches.front()->stableId == L"calculator");
  const auto capabilityItem =
      feathercast::capabilities::Display(*capabilityMatches.front());
  assert(capabilityItem.isCapability);
  assert(capabilityItem.Key() == L"capability:calculator");
  assert(capabilityItem.capability.action.kind ==
         feathercast::app::CapabilityActionKind::SeedQuery);

  feathercast::settings_catalog::CatalogContext context;
  context.clipboardEnabled = false;
  auto privacy = feathercast::settings_catalog::FocusOrder(
      feathercast::app::SettingsCategory::Privacy, context);
  assert(std::find(privacy.begin(), privacy.end(),
                   feathercast::app::HitType::ClipboardLimitDown) ==
         privacy.end());
  context.clipboardEnabled = true;
  privacy = feathercast::settings_catalog::FocusOrder(
      feathercast::app::SettingsCategory::Privacy, context);
  assert(std::find(privacy.begin(), privacy.end(),
                   feathercast::app::HitType::ClipboardLimitDown) !=
         privacy.end());
  const auto* clipboardLimit = feathercast::settings_catalog::Find(
      feathercast::app::HitType::ClipboardLimitDown);
  assert(clipboardLimit);
  context.storageIdle = false;
  assert(!feathercast::settings_catalog::Enabled(*clipboardLimit, context));
  feathercast::app::Settings settingValues;
  settingValues.compactMode = true;
  assert(feathercast::settings_catalog::Checked(
      feathercast::app::HitType::CompactToggle, settingValues));

  feathercast::ui::OverlayState overlay;
  const auto resetEffects = feathercast::ui::OverlayController::ResetForShow(
      overlay, feathercast::app::View::Search);
  assert((resetEffects & feathercast::ui::Effect(
                             feathercast::ui::UiEffect::RequestSearch)) != 0);
  feathercast::ui::OverlayController::SetQuery(overlay, L"terminal");
  assert(overlay.query == L"terminal" && overlay.caret == 8);
  feathercast::ui::OverlayController::MoveCaret(overlay, 4, true);
  assert(overlay.selectionAnchor && *overlay.selectionAnchor == 8);
  feathercast::ui::OverlayController::InsertText(overlay, L"\U0001F600");
  assert(overlay.query == L"term\U0001F600");
  assert(!overlay.selectionAnchor && overlay.selected == 0 &&
         overlay.scroll == 0);
  feathercast::ui::OverlayController::SetQuery(overlay, L"terminal");
  feathercast::ui::OverlayController::Select(overlay, 99, 3);
  assert(overlay.selected == 2);
  feathercast::ui::OverlayController::SetScroll(overlay, 90, 50);
  assert(overlay.scroll == 50);
  feathercast::app::DisplayItem actionTarget;
  actionTarget.app.id = L"terminal";
  actionTarget.app.name = L"Terminal";
  feathercast::ui::OverlayController::EnterAction(
      overlay, actionTarget, L"app:terminal");
  assert(overlay.actionMode && overlay.actionTarget.app.id == L"terminal");
  feathercast::ui::OverlayController::RestoreNavigation(overlay);
  assert(!overlay.actionMode && overlay.selected == 2 && overlay.scroll == 50);
  feathercast::ui::OverlayController::EnterBrowse(
      overlay, feathercast::app::BrowseView::Capabilities, L"cmd:guide");
  assert(overlay.browseView == feathercast::app::BrowseView::Capabilities);
  feathercast::ui::OverlayController::SetQuery(overlay, L"calculator");
  feathercast::ui::OverlayController::EnterBrowse(
      overlay, feathercast::app::BrowseView::Emoji, L"capability:emoji");
  assert(overlay.browseView == feathercast::app::BrowseView::Emoji);
  assert(overlay.navigationStack.size() == 2);
  feathercast::ui::OverlayController::RestoreNavigation(overlay);
  assert(overlay.browseView == feathercast::app::BrowseView::Capabilities);
  assert(overlay.query == L"calculator");
  assert(overlay.pendingNavigationRestore);
  assert(overlay.pendingNavigationRestore->selectedKey ==
         L"capability:emoji");
  feathercast::ui::OverlayController::RestoreNavigation(overlay);
  assert(overlay.browseView == feathercast::app::BrowseView::None);
  assert(overlay.query == L"terminal");

  feathercast::app::ConfirmationDialog confirmation;
  confirmation.title = L"Confirm";
  feathercast::ui::OverlayController::ShowConfirmation(
      overlay, std::move(confirmation));
  assert(overlay.confirmation && overlay.confirmationFocus == 0);
  feathercast::ui::OverlayController::CloseConfirmation(overlay);
  assert(!overlay.confirmation);

  feathercast::ui::SettingsState settings;
  feathercast::ui::SettingsController::SelectCategory(
      settings, feathercast::app::SettingsCategory::Privacy);
  assert(settings.category == feathercast::app::SettingsCategory::Privacy);
  feathercast::ui::SettingsController::MoveFocus(settings, 5, 3);
  assert(settings.focusIndex == 2);
  feathercast::ui::SettingsController::CycleFocus(settings, 1, 3);
  assert(settings.focusIndex == 0);
  feathercast::ui::SettingsController::CycleFocus(settings, -1, 3);
  assert(settings.focusIndex == 2);
  feathercast::ui::SettingsController::BeginShortcutRecording(settings);
  assert(settings.recordingShortcut && settings.pendingShortcut.empty());
  feathercast::ui::SettingsController::SetPendingShortcut(settings,
                                                           L"Ctrl+Space");
  assert(!settings.recordingShortcut &&
         settings.pendingShortcut == L"Ctrl+Space");
  feathercast::ui::SettingsController::CancelShortcutRecording(settings);
  assert(settings.pendingShortcut.empty());
  return 0;
}
