const { app } = require('electron')
const fs = require('fs')
const path = require('path')

const DEFAULTS = {
  shortcut: 'Alt+Space',
  recentApps: [],
  compactMode: false
}

function settingsPath() {
  return path.join(app.getPath('userData'), 'settings.json')
}

function loadSettings() {
  try {
    const raw = fs.readFileSync(settingsPath(), 'utf-8')
    const parsed = JSON.parse(raw)
    return { ...DEFAULTS, ...parsed }
  } catch {
    return { ...DEFAULTS }
  }
}

function saveSettings(settings) {
  const merged = { ...DEFAULTS, ...settings }
  try {
    fs.writeFileSync(settingsPath(), JSON.stringify(merged, null, 2), 'utf-8')
  } catch (err) {
    console.error('Could not save settings:', err)
  }
  return merged
}

module.exports = { DEFAULTS, loadSettings, saveSettings }
