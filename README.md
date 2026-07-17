# FeatherCast

FeatherCast is a lightweight native Windows app launcher. A global shortcut opens a centered search overlay for installed apps and open windows, with real icons, fuzzy search, keyboard-first navigation, and a tray icon.

![FeatherCast Icon](build/icon.png)

## Features

- App search across system and per-user Start Menu shortcuts plus AppsFolder entries
- Open-window switching with foreground restore for minimized windows
- Open-window actions for screen halves, centering, and moving to the next display
- Token-aware fuzzy search ranked for launcher usage
- Optional local file/folder indexing and clipboard history
- Calculator, unit/currency conversion, emoji, symbols, snippets, quicklinks, and web-search prefixes
- Local time, date, ISO-week, Unix-time, and UUID utilities
- Searchable Windows settings plus volume, media playback, and Show Desktop commands
- Searchable “Discover FeatherCast” guide with feature examples and shortcuts
- Native out-of-process plugin host with timeouts and crash isolation
- Lazy shell icon loading with a native PNG icon cache
- Keyboard-first controls: Up/Down, Enter, Ctrl+Shift+Enter, Esc
- Configurable global shortcut, default `Alt+Space`
- Compact mode, Windows accent sync, and custom accent color
- Manual and daily GitHub Release update checks with verified installers
- Background tray menu: Open FeatherCast, Settings, Quit

AI chat and AI provider settings were removed in the native remake.

## Usage

| Action | Key |
| --- | --- |
| Open / close overlay | `Alt+Space` by default |
| Move selection | `Up` / `Down` |
| Launch selected app or focus selected window | `Enter` |
| Launch selected app as administrator | `Ctrl+Shift+Enter` |
| Open result actions | `Tab`, `Right`, or `Ctrl+K` |
| Return from actions or a browse view | `Left` or `Esc` |
| Close overlay | `Esc` |
| Open Settings | Gear button or tray menu |
| Browse feature guide | Search for `help` or `Discover FeatherCast` |

Useful searches include `time`, `date`, `week number`, `unix timestamp`,
`generate uuid`, `display settings`, `volume up`, and `play or pause media`.
Open the action panel on a window result to arrange it, or on a text result to
copy or paste its value.

The tray icon runs in the background. Left-click opens search; right-click opens the menu.

## Development

Prerequisites on Windows:

- A current Visual Studio release with Desktop development with C++
- CMake 3.22 or newer
- Optional: NSIS if you want the CPack NSIS installer target

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
cpack --config build-native/CPackConfig.cmake -C Release
```

The executable is produced as `build-native/Release/FeatherCast.exe` with Visual Studio generators.

Plugin authors should start with [docs/plugin-development.md](docs/plugin-development.md). Release and updater packaging notes are in [docs/releasing.md](docs/releasing.md).

## Settings and Cache

Roaming user configuration is stored under `%APPDATA%\FeatherCast`:

- `settings.json` stores launcher preferences, privacy choices, recent usage, appearance, and update state
- `snippets.json` stores user-authored snippets
- `theme.json` stores optional theme overrides
- `plugins/` contains user-installed native plugins

Machine-local operational data is stored under `%LOCALAPPDATA%\FeatherCast`:

- `feathercast.db` stores the optional file index and user-scoped DPAPI-protected clipboard history
- `icon-cache-native/` stores resolved PNG icons
- `updates/` stores verified update installers
- update and opt-in diagnostic logs

Clipboard history and file indexing are disabled until explicitly enabled in Settings. Settings exposes clipboard retention, file-index limits, custom roots, diagnostics, and data-clearing controls. File indexing defaults to Desktop, Documents, and Downloads when no custom roots are selected.

FeatherCast contacts GitHub Releases for enabled update checks and `open.er-api.com` for cached currency rates. Update installation is disabled in builds that do not contain an allowed Authenticode signer certificate pin. Plugins are native code running with the current user’s permissions and should only be installed from trusted sources.

Older AI-related fields are ignored and are not written back by the native app.

## Icons

The application icon assets are kept in `build/`. Regenerate them with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/gen-icons.ps1
```

## Tech

FeatherCast is now a Win32 C++23 application rendered with Direct2D/DirectWrite. It uses native Windows APIs for tray integration, shell/app discovery, shortcut parsing, low-level keyboard hooks, window enumeration/focus, and shell icon extraction.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
