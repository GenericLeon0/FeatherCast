# LeanCast

A lightweight app launcher for Windows. A global shortcut opens a centered search overlay that lets you find and launch installed programs in an instant — with real application icons and keyboard-first navigation.

![LeanCast Icon](build/icon.png)

## Features

- 🔍 **App search** – scans the Windows Start Menu (system-wide and per-user) for installed programs
- ⚡ **Fuzzy search** – finds matches even with imprecise input, ranked by relevance
- 🖼️ **Real icons** – displays each program's actual icon
- ⌨️ **Keyboard-first** – navigate with ↑/↓, launch with ↵, close with Esc
- 🎛️ **Configurable shortcut** – default `Alt+Space`, freely customizable in Settings
- 📌 **Tray icon** – runs in the background in the system tray

## Usage

| Action | Key |
| --- | --- |
| Open / close overlay | `Alt+Space` (default) |
| Move selection down / up | `↓` / `↑` |
| Launch selected app | `↵` Enter |
| Close overlay | `Esc` |
| Open Settings | Gear ⚙ in the search bar or tray menu |

The **tray icon** (system tray, possibly hidden behind the `^` overflow) offers a right-click menu: *Open LeanCast*, *Settings*, and *Quit*. A left-click opens the search directly.

### Changing the shortcut

Open Settings → click **"Record new shortcut"** → press the desired key combination (at least one modifier: `Ctrl`/`Alt`/`Shift`) → **Save**. The setting persists across restarts. If a shortcut is already taken, an error is shown and the previous one stays active.

## Installation

Built packages are placed under `release/` after building:

- **`LeanCast Setup 0.1.0.exe`** – installer
- **`LeanCast 0.1.0.exe`** – portable (no installation required)

## Development

Prerequisite: [Node.js](https://nodejs.org/) (LTS).

```bash
npm install      # install dependencies
npm run dev      # start Vite + Electron with hot reload
npm run build    # build renderer (React) to dist/
npm run dist     # build portable .exe + installer to release/
```

> **Packaging note:** `electron-builder` unpacks its `winCodeSign` package on Windows builds, which contains macOS symlinks. Creating those requires **Developer Mode** or admin rights. Enable it via *Settings → System → For developers → Developer Mode*. The `dist` script disables code signing (`CSC_IDENTITY_AUTO_DISCOVERY=false`) since no certificate is used.

### Regenerating the icon

The app icon is generated entirely from code (no external assets):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/gen-icons.ps1
```

Produces `build/icon.ico`, `build/icon.png`, and `build/tray.png`.

## Tech

Electron (main process) + React/Vite (renderer). App discovery via the Windows Start Menu, shortcut resolution via `shell.readShortcutLink`, icons via `app.getFileIcon`. Secure IPC via `contextBridge` (no `nodeIntegration` in the renderer).
