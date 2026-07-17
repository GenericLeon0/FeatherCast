# Manual Regression Checklist

Run the checklist at 100%, 150%, and 200% display scaling, first keyboard-only
and then with pointer input.

## Startup and Shutdown

- A first launch creates the same data directories and default settings.
- A second instance activates the existing search window.
- Tray open, settings, and quit actions work; quitting leaves no worker or
  plugin-host process behind.
- Startup registration and the configured global shortcut remain effective.

## Search and Navigation

- Search ordering is unchanged for apps, open windows, commands, files,
  snippets, clipboard, calculations, and conversions.
- Rapid query edits never display results from an older query.
- Arrow keys, Page Up/Down, Home/End, Enter, Escape, Tab, action mode, browse
  views, and back navigation retain their focus and selection behavior.
- IME composition, caret movement, selection, clipboard editing, and empty-query
  recent results behave as before.

## Pointer and Accessibility

- Hover, wheel scrolling, press/release activation, settings controls, action
  menus, confirmation buttons, and clicks outside the overlay behave as before.
- Moving off a pressed target before release does not activate it.
- Narrator/UI Automation names, roles, focus, actions, and bounds match the
  visible controls.

## Settings and Persistence

- Every settings category has the same focus order and controls.
- Shortcut recording, accent selection, quicklinks, folder selection, and all
  maintenance actions work.
- Settings persist across restart without changing paths or unrelated fields.
- A corrupt settings file is preserved/recovered; a future schema version is
  preserved and automatic saving is blocked.
- Clipboard retention, file indexing, clear actions, and SQLite failure notices
  remain correct.

## Discovery, Launch, and Updates

- Start Menu, AppsFolder, open-window, and opt-in file discovery produce the
  same entries and deduplication.
- App launch, run as administrator, window focus, URLs, folders, commands, and
  plugin activation retain their notifications and recent-item tracking.
- Icons load lazily, survive device recreation, and can be cleared/rebuilt.
- Automatic/manual update checks, failure notices, SHA-256 verification,
  Authenticode verification, installer launch, and currency conversion retain
  their existing URLs, timeouts, and visible behavior.

## Visuals

- Overlay/settings dimensions, spacing, colors, typography, selection pill,
  animations, confirmation dialogs, compact mode, high contrast, and DPI
  changes match the baseline.
- Minimize/restore, monitor changes, and graphics device loss recreate resources
  without stale hit regions or crashes.

## Feature-readiness baseline (2026-07-17)

Automated baseline:

- Clean warnings-as-errors builds completed in Debug and Release.
- All 12 CTest targets passed in Debug and Release.
- Release search p95 measured 7.51 ms at 5,000 items and 20.05 ms at
  50,000 items, within the 10 ms and 50 ms budgets.
- Catalog tests validate unique capability, command, and setting IDs; complete
  setting accessibility metadata; typed guide actions; controller navigation,
  query editing, selection, confirmation, focus, disabled controls, and shortcut
  recording. Interaction tests block activation while results are stale.

Current-scale live smoke:

- The empty query keeps pinned/recent ordering and appends Explore -> Discover
  FeatherCast after the normal empty-query sections.
- The guide opens with the feature-search placeholder, filters by capability,
  opens a nested browse view, and restores the guide query on Escape.
- Seed-query actions produce normal ranked results without a separate editor.
- The settings window renders from the descriptor-backed metadata and retains
  keyboard/pointer focus behavior.

The 150% and 200% passes, IME-specific input, Narrator/Accessibility Insights,
forced graphics-device recreation, shutdown cleanup, and second-instance checks
remain release-gate manual work. Current-build screenshots were captured during
the smoke pass but are not checked in because launcher results can expose local
application, file, and clipboard names.
