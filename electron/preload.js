const { contextBridge, ipcRenderer } = require('electron')

// Secure API for the renderer. No direct Node access.
contextBridge.exposeInMainWorld('leancast', {
  listApps: () => ipcRenderer.invoke('apps:list'),
  getIcon: (filePath) => ipcRenderer.invoke('apps:icon', filePath),
  launch: (filePath) => ipcRenderer.invoke('apps:launch', filePath),
  // List open windows or bring one to the foreground.
  listWindows: () => ipcRenderer.invoke('windows:list'),
  focusWindow: (hwnd) => ipcRenderer.invoke('windows:focus', hwnd),
  getSettings: () => ipcRenderer.invoke('settings:get'),
  setShortcut: (shortcut) => ipcRenderer.invoke('settings:setShortcut', shortcut),
  // Toggle compact mode; reports the required window height in compact mode.
  setCompact: (on) => ipcRenderer.invoke('settings:setCompact', on),
  setWindowHeight: (h) => ipcRenderer.invoke('window:setHeight', h),
  hide: () => ipcRenderer.invoke('window:hide'),
  // Switch view: 'search' | 'settings'
  setView: (view) => ipcRenderer.invoke('window:setView', view),
  // Fired every time the overlay opens; delivers the desired view.
  onShow: (cb) => {
    const handler = (_e, view) => cb(view)
    ipcRenderer.on('window:show', handler)
    return () => ipcRenderer.removeListener('window:show', handler)
  }
})
