# Manual Regression Checklist

Run the checklist at 100%, 150%, and 200% display scaling, first keyboard-only
and then with pointer input.

## Startup and Shutdown

- A first launch creates the same data directories and default settings.
- A second instance activates the existing search window.
- Tray open, settings, and quit actions work; quitting leaves no worker or
  plugin-host process behind.
- Startup registration and the configured global shortcut remain effective.
- With `Super` configured as the shortcut, either Windows key opens the overlay
  once on key-down from the desktop, an open Start menu, and Task Manager
  running normally, elevated, or always on top. The overlay receives keyboard
  focus, remains above Task Manager, and Start stays closed after key-up.
- Holding the Windows key does not retrigger the overlay. While `Super` is the
  shortcut, Windows-key chords such as `Win+E` are consumed without leaking
  text or commands; selecting a different shortcut restores normal Windows-key
  and Windows-chord behavior.
- With Notepad, Windows Terminal, and Explorer, open or focus a new window and
  invoke the overlay after 0, 50, 150, 300, and 750 ms. Closing it with `Super`
  restores the newest relevant window rather than the previously active one.
  Repeat with Full, Reduced, and Off animations, with both a short tap and a
  held Windows key, and once with `Alt+Space`.
- If another window becomes foreground during the overlay's close animation,
  FeatherCast leaves that window focused. Passive focus loss, launching a
  result, and opening Settings or Volume never restore an older window.

## Search and Navigation

- Search ordering follows the balanced match tiers: exact name, name prefix or
  acronym, name boundary, field prefix, typo, then general fuzzy/path matches.
  Pinning and usage reorder items only inside the same tier.
- Rapid query edits never display results from an older query.
- Results dim while a generation is pending; navigation pressed during that
  interval applies to the newly published generation, while Enter and pointer
  activation remain blocked.
- Arrow keys, Page Up/Down, Home/End, Enter, Escape, Tab, action mode, browse
  views, and back navigation retain their focus and selection behavior.
- IME composition, caret movement, selection, clipboard editing, and empty-query
  recent results behave as before.
- Typing `@` offers all scopes; Tab/Enter activates the highlighted scope and
  Escape removes a complete leading scope before closing the overlay.
- `@apps`, `@windows`, `@commands`, `@clipboard`, and `@snippets` contain only
  their intended result type. Root search never displays content-only matches.
- Empty `@files` is newest-first. Name/path results precede content matches and
  the same normalized Windows path never appears twice.

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
- Content indexing requires its separate disclosure/opt-in and can be disabled
  without removing searchable file metadata. Add, Remove, Use Defaults, Rebuild,
  status, and Delete File Index work for the configured root list.
- Deep trees, create/write/rename/delete bursts, unavailable roots, permission
  failures, watcher overflow reconciliation, Sleep/Resume, and shutdown converge
  to the correct index without following hidden/system/reparse entries.
- Settings > Library lists snippets and quicklinks and opens the native manager
  on the requested tab. Add, edit, cancel, delete confirmation, reload, and
  Open File work with keyboard, pointer, IME, Narrator, and multiline text.
- Duplicate keywords are marked and cannot be introduced by a new edit.
  Invalid or externally changed snippets.json is never overwritten.
- Extensions lists each plugin's version and Available, Degraded, or
  Unavailable state, including the latest failure reason when present.

## Discovery, Launch, and Updates

- Start Menu, AppsFolder, open-window, and opt-in file discovery produce the
  same entries and deduplication.
- App launch, run as administrator, window focus, URLs, folders, commands, and
  plugin activation retain their notifications and recent-item tracking.
- Icons load lazily, survive device recreation, and can be cleared/rebuilt.
- Automatic/manual update checks, failure notices, SHA-256 verification,
  Authenticode verification, installer launch, and currency conversion retain
  their existing URLs, timeouts, and visible behavior.

## Visuals and Motion

- After a cold process start, the first overlay opening begins at the same
  scale/opacity and remains as smooth as subsequent openings; no initialization
  frame flashes or jumps ahead.
- With the PC awake and the overlay hidden for 1, 5, and 15 minutes, the first
  Full-animation opening remains display-paced and does not stutter or jump.
- At 60 Hz, 120 Hz, and 144 Hz, overlay/settings/volume openings, result
  transitions, the selection pill, settings pages and switches, confirmations,
  and volume changes remain paced to the display without visible snapping.
- Wheel and precision-trackpad deltas scroll both results and settings smoothly;
  repeated input retargets from the currently rendered position.
- Compact and confirmation height changes, settings resizing, and opening
  bounds finish at the correct top-anchored or centered final rectangle.
- Disabling app or Windows animations and enabling high contrast immediately
  snaps every active transition to its final state.
- Minimize/restore, monitor changes, and graphics device loss recreate resources
  without stale hit regions or crashes.

## File Preview

- `Ctrl+Space` and the Preview result action open/close the pane while keyboard
  focus and text input stay in search.
- Wide monitors show the 420-DIP side pane; small monitors replace only the
  results area. Verify at 100%, 150%, and 200% DPI.
- Selection changes update after the debounce and never show stale content.
  `Ctrl+PageUp/PageDown` and the wheel over the pane scroll only the preview.
- Supported text is bounded and centered on the first query match. Supported
  images respect byte/pixel/dimension limits; corrupt or deleted files fail
  safely. Other formats expose metadata only.
- Device loss recreates image resources, and High Contrast, Reduced Motion,
  Narrator, keyboard, and pointer paths remain usable.

## Search & Files automated baseline (2026-07-18)

- Warnings-as-errors Debug and Release builds completed; all 16 CTest targets
  passed in both configurations.
- Release root-search p95 measured 9.71 ms at 5,000 items and 36.91 ms at
  50,000 items, within the unchanged 10 ms and 50 ms budgets.
- Warm `@files` full-text search p95 measured 31.41 ms with 50,000 synthetic
  documents, within the 75 ms budget.
- ZIP and NSIS packages plus app/plugin-host self-tests completed successfully.
- Automated Search & Files coverage includes scope parsing/filtering/suggestions,
  deep recursive indexing, watcher create/rename/delete reconciliation, encoding
  and binary detection, schema v2-to-v3 backup/migration, future-schema blocking,
  contentless FTS upsert/delete, name-over-content deduplication, and bounded text
  preview/deleted-file handling.

## Smoothness-pass automated baseline (2026-07-17)

- The Release build completed and all 14 CTest targets passed.
- Release search p95 measured 7.95 ms at 5,000 items and 23.02 ms at 50,000
  items for normal queries, plus 7.29 ms and 23.80 ms for typo-heavy queries.
- Core tests cover match tiers, personalization boundaries, bounded
  Damerau-Levenshtein equivalence, limited top-k equivalence, and cancellation.
- Interaction tests cover interruption-safe scalar/bounds motion, opening
  anchors, partial wheel deltas, pending navigation, and reduced-motion snaps.

## Earlier feature-readiness baseline (2026-07-17)

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
