const { app, BrowserWindow, globalShortcut, ipcMain, shell, screen, Tray, Menu, nativeImage } = require('electron')
const path = require('path')
const { listApps, getIcon, listWindows, focusWindow } = require('./apps')
const { loadSettings, saveSettings } = require('./settings')

const DEV_URL = process.env.VITE_DEV_SERVER_URL

const WIN_WIDTH = 720
const WIN_HEIGHT = 470
// Height in compact mode while only the search bar is visible; the renderer
// reports the actual needed height afterwards via 'window:setHeight'.
const COMPACT_BASE_HEIGHT = 58
const RECENT_LIMIT = 8

const ICON_PATH = path.join(__dirname, '..', 'build', 'icon.ico')
const TRAY_ICON_PATH = path.join(__dirname, '..', 'build', 'tray.png')

let win = null
let tray = null
let settings = loadSettings()
let currentShortcut = null
let isQuitting = false

// Allow only a single instance.
const gotLock = app.requestSingleInstanceLock()
if (!gotLock) {
  app.quit()
}

function createWindow() {
  win = new BrowserWindow({
    width: WIN_WIDTH,
    height: WIN_HEIGHT,
    icon: ICON_PATH,
    show: false,
    frame: false,
    transparent: true,
    resizable: false,
    movable: false,
    skipTaskbar: true,
    alwaysOnTop: true,
    fullscreenable: false,
    backgroundColor: '#00000000',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  win.setAlwaysOnTop(true, 'screen-saver')

  if (DEV_URL) {
    win.loadURL(DEV_URL)
  } else {
    win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'))
  }

  // Hide on focus loss (except in dev with DevTools open).
  win.on('blur', () => {
    if (win && !win.webContents.isDevToolsOpened()) win.hide()
  })
}

function createTray() {
  let image = nativeImage.createFromPath(TRAY_ICON_PATH)
  if (image.isEmpty()) image = nativeImage.createFromPath(ICON_PATH)
  tray = new Tray(image)
  tray.setToolTip('LeanCast')

  const menu = Menu.buildFromTemplate([
    { label: 'Open LeanCast', click: () => showWindow('search') },
    { label: 'Settings', click: () => showWindow('settings') },
    { type: 'separator' },
    {
      label: 'Quit',
      click: () => {
        isQuitting = true
        app.quit()
      }
    }
  ])
  tray.setContextMenu(menu)
  // Left-click opens the search window.
  tray.on('click', () => showWindow('search'))
}

function positionWindow() {
  const cursor = screen.getCursorScreenPoint()
  const display = screen.getDisplayNearestPoint(cursor)
  const { x, y, width, height } = display.workArea
  // use current window width (height can vary in compact mode)
  const [winW] = win.getSize()
  const winX = Math.round(x + (width - winW) / 2)
  // slightly above center – like Raycast/Spotlight
  const winY = Math.round(y + height * 0.22)
  win.setPosition(winX, winY)
}

// Set window size for the current mode. Default = fixed size; Compact =
// search bar only (the renderer grows the height via 'window:setHeight').
function applyWindowSize() {
  if (!win) return
  const height = settings.compactMode ? COMPACT_BASE_HEIGHT : WIN_HEIGHT
  win.setSize(WIN_WIDTH, height)
}

function showWindow(view = 'search') {
  if (!win) return
  // Settings view always needs full height; otherwise use compact setting.
  if (view === 'settings') win.setSize(WIN_WIDTH, WIN_HEIGHT)
  else applyWindowSize()
  positionWindow()
  win.show()
  win.focus()
  win.webContents.send('window:show', view)
}

function toggleWindow() {
  if (!win) return
  if (win.isVisible()) {
    win.hide()
  } else {
    showWindow('search')
  }
}

function registerShortcut(accelerator) {
  if (currentShortcut) {
    globalShortcut.unregister(currentShortcut)
    currentShortcut = null
  }
  try {
    const ok = globalShortcut.register(accelerator, toggleWindow)
    if (ok) {
      currentShortcut = accelerator
      return { ok: true }
    }
    return { ok: false, error: 'Could not register shortcut (may already be in use).' }
  } catch (err) {
    return { ok: false, error: String(err && err.message ? err.message : err) }
  }
}

// Prepend the launched path to the recently used apps list.
function trackRecent(filePath) {
  if (!filePath) return
  const prev = Array.isArray(settings.recentApps) ? settings.recentApps : []
  const recentApps = [filePath, ...prev.filter((p) => p !== filePath)].slice(0, RECENT_LIMIT)
  settings = saveSettings({ ...settings, recentApps })
}

// ---- IPC ----
ipcMain.handle('apps:list', () => listApps())
ipcMain.handle('apps:icon', (_e, filePath) => getIcon(filePath))
ipcMain.handle('apps:launch', async (_e, filePath) => {
  if (win) win.hide()
  const err = await shell.openPath(filePath)
  if (!err) trackRecent(filePath)
  return { ok: !err, error: err || null }
})
ipcMain.handle('windows:list', () => listWindows())
ipcMain.handle('windows:focus', (_e, hwnd) => {
  if (win) win.hide()
  return focusWindow(hwnd)
})
ipcMain.handle('settings:get', () => settings)
ipcMain.handle('settings:setShortcut', (_e, shortcut) => {
  const result = registerShortcut(shortcut)
  if (result.ok) {
    settings = saveSettings({ ...settings, shortcut })
  } else {
    // restore previous shortcut
    registerShortcut(settings.shortcut)
  }
  return result
})
ipcMain.handle('settings:setCompact', (_e, on) => {
  // Save only – the size takes effect next time the search overlay opens
  // (or via 'window:setHeight' when the renderer switches back to search).
  // Don't resize here or the open settings view will shrink.
  settings = saveSettings({ ...settings, compactMode: !!on })
  return settings
})
ipcMain.handle('window:setHeight', (_e, h) => {
  if (!win || !settings.compactMode) return
  const cursor = screen.getCursorScreenPoint()
  const display = screen.getDisplayNearestPoint(cursor)
  const maxH = Math.round(display.workArea.height * 0.7)
  const height = Math.max(COMPACT_BASE_HEIGHT, Math.min(Math.round(h), maxH))
  win.setSize(WIN_WIDTH, height)
})
ipcMain.handle('window:hide', () => {
  if (win) win.hide()
})
ipcMain.handle('window:setView', () => {
  // Placeholder for future view logic; currently handled entirely in the renderer.
})

if (gotLock) {
  app.on('second-instance', () => showWindow('search'))

  app.whenReady().then(() => {
    createWindow()
    createTray()
    const result = registerShortcut(settings.shortcut)
    if (!result.ok) {
      // Fallback if the saved shortcut is already taken.
      registerShortcut('Alt+Space')
    }
  })

  app.on('before-quit', () => {
    isQuitting = true
  })

  app.on('will-quit', () => {
    globalShortcut.unregisterAll()
    if (tray) tray.destroy()
  })

  // Keep the app running in the background even when the window is hidden.
  app.on('window-all-closed', (e) => {
    e.preventDefault()
  })
}
