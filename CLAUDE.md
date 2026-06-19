# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What is LeanCast?

LeanCast is a **lightweight app launcher for Windows** in the style of a fast Spotlight/Command Palette clone: a global shortcut opens a centered search overlay to find and launch installed programs. Current feature set: **app search** and **configurable global shortcut**, plus a background tray icon.

## Tech Stack

- **Electron** – main process (window, global hotkey, tray, app discovery, IPC)
- **React + Vite** – renderer (search UI, settings)
- Windows-only target. No TypeScript, plain JS/JSX.

## Architecture

```
electron/
  main.js       # BrowserWindow, globalShortcut, Tray, IPC handlers, lifecycle
  preload.js    # contextBridge API (no nodeIntegration in the renderer)
  apps.js       # scan Start Menu, resolve icons via shortcut target
  settings.js   # JSON persistence in app.getPath('userData')/settings.json
src/
  App.jsx       # search overlay (list, keyboard nav, lazy icon loading)
  Settings.jsx  # shortcut recorder (KeyboardEvent -> Electron Accelerator)
  fuzzy.js      # lightweight subsequence fuzzy search with scoring
  styles.css    # dark, rounded overlay design
scripts/
  gen-icons.ps1 # generates build/icon.ico, icon.png, tray.png from code
build/          # generated icons (app, window, tray icon)
```

### Data flow

1. **Main** scans the Windows Start Menu (`apps.js`) on startup and keeps the list in memory.
2. **Renderer** fetches the list via IPC (`apps:list`), filters locally with `fuzzy.js`.
3. **Icons** are lazy-loaded per visible hit via `apps:icon`.
4. **Hotkey** (`globalShortcut`) toggles the window; the tray offers Open/Settings/Quit.
5. **Settings** are stored as JSON in `userData` and loaded on startup.

### Key implementation details

- **Icon resolution:** `app.getFileIcon` returns only a generic shortcut icon for `.lnk` files.
  Therefore `apps.js` first resolves the **target** (exe/icon) via `shell.readShortcutLink()` and
  fetches the icon from that (`size: 'large'`). Generic mini-icons (~546 bytes) are discarded;
  otherwise the letter placeholder in the renderer is used.
- **Window:** frameless, transparent, `alwaysOnTop`, `skipTaskbar`, hidden by default. On blur and
  `Esc` the window is `hide()`d (not `close()`d) → instant re-opening.
- **Show event:** Main sends `window:show` with the desired view (`'search'` | `'settings'`); the
  renderer sets the view + resets state in one handler (no separate show/view events – avoids race).
- **Single instance:** `app.requestSingleInstanceLock()`; a second instance just opens the overlay.
- **Quit:** `isQuitting` flag + `app.quit()` from the tray; `window-all-closed` is intentionally
  suppressed so the app keeps running in the background.

## Commands

```bash
npm run dev      # Vite + Electron (hot reload). VITE_DEV_SERVER_URL controls dev vs. file://
npm run build    # build renderer to dist/
npm run dist     # release/: portable .exe + NSIS installer
```

## Build gotchas (Windows)

- `npm run dist` disables code signing via `CSC_IDENTITY_AUTO_DISCOVERY=false`.
- `electron-builder` unpacks `winCodeSign`, which contains macOS symlinks → on Windows only
  unpackable with **Developer Mode** enabled or with admin rights. Permanent fix: enable Developer Mode.
  Workaround without admin: unpack the package once manually to
  `%LOCALAPPDATA%\electron-builder\Cache\winCodeSign\winCodeSign-2.6.0` (ignore the two unused
  `.dylib` symlinks).
- Before a rebuild, kill any running `LeanCast.exe`/`electron.exe` – otherwise "Access is denied"
  when overwriting `release/win-unpacked`.

## Conventions

- Keep UI text and code comments in **English**.
- The renderer has no direct Node access; expose new main-process functions through `preload.js` as a
  narrow `contextBridge` API.
- Do not place icons manually – regenerate them via `scripts/gen-icons.ps1`.
