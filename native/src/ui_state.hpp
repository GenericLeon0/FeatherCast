#pragma once

#include "app_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::ui {

enum class UiEffect : std::uint32_t {
  None = 0,
  RequestSearch = 1u << 0,
  Invalidate = 1u << 1,
  EnsureSelectionVisible = 1u << 2,
  PersistSettings = 1u << 3,
  RefreshData = 1u << 4,
  FocusWindow = 1u << 5,
  CloseView = 1u << 6,
};

using UiEffects = std::uint32_t;

constexpr UiEffects Effect(UiEffect effect) {
  return static_cast<UiEffects>(effect);
}

constexpr UiEffects operator|(UiEffect left, UiEffect right) {
  return Effect(left) | Effect(right);
}

constexpr bool HasEffect(UiEffects effects, UiEffect effect) {
  return (effects & Effect(effect)) != 0;
}

struct NavigationState {
  app::View view = app::View::Search;
  bool actionMode = false;
  app::BrowseView browseView = app::BrowseView::None;
  std::wstring query;
  std::size_t caret = 0;
  std::optional<std::size_t> selectionAnchor;
  int selected = 0;
  int scroll = 0;
  std::wstring selectedKey;
};

struct OverlayState {
  app::View view = app::View::Search;
  std::wstring query;
  std::wstring imeComposition;
  std::size_t caret = 0;
  std::optional<std::size_t> selectionAnchor;
  int selected = 0;
  int scroll = 0;
  bool actionMode = false;
  app::DisplayItem actionTarget;
  app::BrowseView browseView = app::BrowseView::None;
  std::optional<app::ConfirmationDialog> confirmation;
  std::optional<app::StatusMessage> status;
  int confirmationFocus = 0;
  int confirmationHover = -1;
  std::vector<NavigationState> navigationStack;
  std::optional<NavigationState> pendingNavigationRestore;
};

struct SettingsState {
  app::SettingsCategory category = app::SettingsCategory::General;
  int focusIndex = 0;
  float scroll = 0.0f;
  bool recordingShortcut = false;
  std::wstring pendingShortcut;
  int hover = -1;
};

class OverlayController {
 public:
  static UiEffects ResetForShow(OverlayState& state, app::View view) {
    state.view = view;
    state.query.clear();
    state.imeComposition.clear();
    state.caret = 0;
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.actionMode = false;
    state.actionTarget = {};
    state.browseView = app::BrowseView::None;
    state.confirmation.reset();
    state.status.reset();
    state.confirmationFocus = 0;
    state.confirmationHover = -1;
    state.navigationStack.clear();
    state.pendingNavigationRestore.reset();
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects SetQuery(OverlayState& state, std::wstring query) {
    state.query = std::move(query);
    state.imeComposition.clear();
    state.caret = state.query.size();
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects Select(OverlayState& state, int index, int itemCount,
                          bool ensureVisible = true) {
    state.selected = itemCount <= 0 ? 0 : std::clamp(index, 0, itemCount - 1);
    return ensureVisible
               ? Effect(UiEffect::Invalidate) |
                     Effect(UiEffect::EnsureSelectionVisible)
               : Effect(UiEffect::Invalidate);
  }

  static UiEffects SetScroll(OverlayState& state, int scroll, int maximum) {
    state.scroll = std::clamp(scroll, 0, std::max(0, maximum));
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects ResetResultPosition(OverlayState& state) {
    state.selected = 0;
    state.scroll = 0;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects RestoreResultPosition(OverlayState& state, int selected,
                                         int scroll) {
    state.selected = std::max(0, selected);
    state.scroll = std::max(0, scroll);
    return Effect(UiEffect::Invalidate);
  }

  static void ClampCaret(OverlayState& state) {
    state.caret = std::min(state.caret, state.query.size());
    if (state.selectionAnchor) {
      *state.selectionAnchor =
          std::min(*state.selectionAnchor, state.query.size());
    }
  }

  static std::optional<std::pair<std::size_t, std::size_t>> SelectionRange(
      const OverlayState& state) {
    if (!state.selectionAnchor || *state.selectionAnchor == state.caret) {
      return std::nullopt;
    }
    return std::minmax(*state.selectionAnchor, state.caret);
  }

  static UiEffects MoveCaret(OverlayState& state, std::size_t next,
                             bool extendSelection) {
    next = std::min(next, state.query.size());
    if (extendSelection) {
      if (!state.selectionAnchor) state.selectionAnchor = state.caret;
    } else {
      state.selectionAnchor.reset();
    }
    state.caret = next;
    return Effect(UiEffect::Invalidate);
  }

  static bool DeleteSelection(OverlayState& state) {
    const auto range = SelectionRange(state);
    if (!range) return false;
    state.query.erase(range->first, range->second - range->first);
    state.caret = range->first;
    state.selectionAnchor.reset();
    return true;
  }

  static UiEffects InsertText(OverlayState& state, const std::wstring& text,
                              std::size_t maxCharacters = 4096) {
    ClampCaret(state);
    DeleteSelection(state);
    const std::size_t room = state.query.size() < maxCharacters
                                 ? maxCharacters - state.query.size()
                                 : 0;
    const std::wstring clipped = text.substr(0, room);
    state.query.insert(state.caret, clipped);
    state.caret += clipped.size();
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static void PushNavigation(OverlayState& state,
                             std::wstring selectedKey = {}) {
    state.navigationStack.push_back(
        {state.view, state.actionMode, state.browseView, state.query,
         state.caret, state.selectionAnchor, state.selected, state.scroll,
         std::move(selectedKey)});
  }

  static UiEffects EnterAction(OverlayState& state,
                               const app::DisplayItem& target,
                               std::wstring selectedKey = {}) {
    PushNavigation(state, std::move(selectedKey));
    state.actionMode = true;
    state.actionTarget = target;
    state.browseView = app::BrowseView::None;
    ClearQueryAndSelection(state);
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects EnterBrowse(OverlayState& state, app::BrowseView view,
                               std::wstring selectedKey = {}) {
    PushNavigation(state, std::move(selectedKey));
    state.actionMode = false;
    state.browseView = view;
    ClearQueryAndSelection(state);
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects RestoreNavigation(OverlayState& state) {
    state.imeComposition.clear();
    state.status.reset();
    if (state.navigationStack.empty()) {
      state.actionMode = false;
      state.browseView = app::BrowseView::None;
      state.actionTarget = {};
      ClearQueryAndSelection(state);
      state.pendingNavigationRestore.reset();
    } else {
      NavigationState restored = std::move(state.navigationStack.back());
      state.navigationStack.pop_back();
      state.view = restored.view;
      state.actionMode = restored.actionMode;
      state.browseView = restored.browseView;
      state.query = restored.query;
      state.caret = std::min(restored.caret, state.query.size());
      state.selectionAnchor = restored.selectionAnchor;
      if (state.selectionAnchor) {
        *state.selectionAnchor =
            std::min(*state.selectionAnchor, state.query.size());
      }
      state.selected = restored.selected;
      state.scroll = restored.scroll;
      state.pendingNavigationRestore = std::move(restored);
    }
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects ShowConfirmation(
      OverlayState& state, app::ConfirmationDialog confirmation) {
    state.confirmation = std::move(confirmation);
    state.confirmationFocus = 0;
    state.confirmationHover = -1;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects CloseConfirmation(OverlayState& state) {
    state.confirmation.reset();
    state.confirmationFocus = 0;
    state.confirmationHover = -1;
    return Effect(UiEffect::Invalidate);
  }

 private:
  static void ClearQueryAndSelection(OverlayState& state) {
    state.query.clear();
    state.imeComposition.clear();
    state.caret = 0;
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
  }
};

class SettingsController {
 public:
  static UiEffects Open(SettingsState& state, int categoryIndex) {
    state.recordingShortcut = false;
    state.pendingShortcut.clear();
    state.hover = -1;
    state.focusIndex = std::max(0, categoryIndex);
    state.scroll = 0.0f;
    return UiEffect::Invalidate | UiEffect::FocusWindow;
  }

  static UiEffects Close(SettingsState& state) {
    state.recordingShortcut = false;
    state.pendingShortcut.clear();
    return UiEffect::CloseView | UiEffect::Invalidate;
  }

  static UiEffects SelectCategory(SettingsState& state,
                                  app::SettingsCategory category) {
    state.category = category;
    state.focusIndex = 0;
    state.scroll = 0.0f;
    state.recordingShortcut = false;
    state.pendingShortcut.clear();
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects MoveFocus(SettingsState& state, int delta, int itemCount) {
    if (itemCount <= 0) {
      state.focusIndex = 0;
    } else {
      state.focusIndex =
          std::clamp(state.focusIndex + delta, 0, itemCount - 1);
    }
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects SetFocus(SettingsState& state, int index, int itemCount) {
    state.focusIndex = itemCount <= 0 ? 0 : std::clamp(index, 0, itemCount - 1);
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects CycleFocus(SettingsState& state, int delta,
                              int itemCount) {
    if (itemCount <= 0) {
      state.focusIndex = 0;
    } else {
      state.focusIndex =
          (state.focusIndex + delta + itemCount) % itemCount;
    }
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects SetScroll(SettingsState& state, float scroll,
                             float maximum) {
    state.scroll = std::clamp(scroll, 0.0f, std::max(0.0f, maximum));
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects ScrollBy(SettingsState& state, float delta,
                            float maximum) {
    return SetScroll(state, state.scroll + delta, maximum);
  }

  static UiEffects BeginShortcutRecording(SettingsState& state) {
    state.pendingShortcut.clear();
    state.recordingShortcut = true;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects SetPendingShortcut(SettingsState& state,
                                      std::wstring shortcut) {
    state.pendingShortcut = std::move(shortcut);
    state.recordingShortcut = false;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects CancelShortcutRecording(SettingsState& state) {
    state.recordingShortcut = false;
    state.pendingShortcut.clear();
    return Effect(UiEffect::Invalidate);
  }
};

}  // namespace feathercast::ui
