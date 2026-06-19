const { app, shell } = require('electron')
const fs = require('fs')
const path = require('path')
const { execFile } = require('child_process')

// Default Start Menu directories on Windows.
function startMenuDirs() {
  const dirs = []
  const programData = process.env.ProgramData || 'C:\\ProgramData'
  const appData = process.env.APPDATA
  dirs.push(path.join(programData, 'Microsoft', 'Windows', 'Start Menu', 'Programs'))
  if (appData) {
    dirs.push(path.join(appData, 'Microsoft', 'Windows', 'Start Menu', 'Programs'))
  }
  return dirs.filter((d) => {
    try {
      return fs.statSync(d).isDirectory()
    } catch {
      return false
    }
  })
}

// Recursively collect all .lnk files.
function walkLnks(dir, out) {
  let entries
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true })
  } catch {
    return
  }
  for (const entry of entries) {
    const full = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      walkLnks(full, out)
    } else if (entry.isFile() && entry.name.toLowerCase().endsWith('.lnk')) {
      out.push(full)
    }
  }
}

let cache = null

// App list: { id, name, path }. De-duplicated by lowercase name.
function listApps() {
  if (cache) return cache
  const lnks = []
  for (const dir of startMenuDirs()) {
    walkLnks(dir, lnks)
  }

  const seen = new Set()
  const apps = []
  for (const lnkPath of lnks) {
    const name = path.basename(lnkPath, '.lnk')
    const key = name.toLowerCase()
    // Filter out uninstallers and similar entries
    if (/^(uninstall|deinstall|readme|hilfe|help|website|homepage)\b/i.test(name)) continue
    if (seen.has(key)) continue
    seen.add(key)
    apps.push({ id: lnkPath, name, path: lnkPath })
  }

  apps.sort((a, b) => a.name.localeCompare(b.name))
  cache = apps
  return apps
}

function refreshApps() {
  cache = null
  return listApps()
}

// Minimum edge length for an icon to be considered "real" (generic mini icons
// are 16px – we don't want those; we'd rather show the letter placeholder).
const MIN_ICON_SIZE = 32

// Fetch an icon candidate from a path; returns a data URL or null.
// Tries multiple sizes because some exes return an empty image at 'large'.
async function iconFromPath(target) {
  if (!target) return null
  for (const size of ['large', 'normal']) {
    try {
      const img = await app.getFileIcon(target, { size })
      if (img && !img.isEmpty()) {
        const { width, height } = img.getSize()
        if (Math.min(width, height) >= MIN_ICON_SIZE) return img.toDataURL()
      }
    } catch {
      /* try next size / next candidate */
    }
  }
  return null
}

// Fetch the real app icon. .lnk files return only a generic shortcut icon via
// getFileIcon – so we resolve the target (exe/icon) first.
// Also works directly with an exe path (e.g. for open windows).
async function getIcon(targetPath) {
  const candidates = []
  try {
    const link = shell.readShortcutLink(targetPath)
    if (link) {
      // explicitly set shortcut icon – may include an index suffix
      // like "C:\\...\\brave.exe,0" (strip the ",0" for getFileIcon).
      if (link.icon) {
        const iconPath = link.icon.replace(/,\s*-?\d+\s*$/, '')
        if (/\.(ico|exe|dll)$/i.test(iconPath)) candidates.push(iconPath)
      }
      // the actual target program
      if (link.target) candidates.push(link.target)
      // working directory – some targets are stubs without an icon
      if (link.cwd && link.target) {
        candidates.push(path.join(link.cwd, path.basename(link.target)))
      }
    }
  } catch {
    /* readShortcutLink is only available on Windows / for .lnk files */
  }
  candidates.push(targetPath) // fallback: the file itself (exe or .lnk)

  for (const target of candidates) {
    const url = await iconFromPath(target)
    if (url) return url
  }
  return null
}

// Invoke PowerShell and return stdout as text.
function runPowerShell(script) {
  return new Promise((resolve, reject) => {
    execFile(
      'powershell',
      ['-NoProfile', '-NonInteractive', '-Command', script],
      { windowsHide: true, maxBuffer: 4 * 1024 * 1024 },
      (err, stdout) => {
        if (err) reject(err)
        else resolve(stdout)
      }
    )
  })
}

// List open windows (visible top-level windows with a title).
// Returns: { kind:'window', pid, hwnd, name, exe }[]
async function listWindows() {
  const script =
    "Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle } | " +
    'Select-Object Id, ProcessName, MainWindowTitle, ' +
    '@{Name="Hwnd";Expression={$_.MainWindowHandle.ToInt64()}}, Path | ConvertTo-Json -Compress'
  try {
    const out = await runPowerShell(script)
    if (!out || !out.trim()) return []
    let parsed = JSON.parse(out)
    if (!Array.isArray(parsed)) parsed = [parsed] // single object when only 1 window
    return parsed
      .map((p) => ({
        kind: 'window',
        pid: p.Id,
        hwnd: p.Hwnd,
        name: p.MainWindowTitle,
        exe: p.Path || null,
        processName: p.ProcessName || ''
      }))
      .filter((w) => {
        if (!w.name) return false
        // hide LeanCast's own Electron window
        const proc = (w.processName || '').toLowerCase()
        if (proc === 'electron' || proc === 'leancast') return false
        if (/leancast/i.test(w.name)) return false
        return true
      })
  } catch {
    return []
  }
}

// Bring a window to the foreground by its handle (works even if minimized).
async function focusWindow(hwnd) {
  if (!hwnd) return { ok: false, error: 'No window handle.' }
  const script = `
$sig = @'
using System;
using System.Runtime.InteropServices;
public class LeanWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
}
'@
Add-Type -TypeDefinition $sig
$h = [IntPtr]::new(${Number(hwnd)})
[LeanWin]::ShowWindowAsync($h, 9) | Out-Null
[LeanWin]::SetForegroundWindow($h) | Out-Null
`
  try {
    await runPowerShell(script)
    return { ok: true }
  } catch (err) {
    return { ok: false, error: String(err && err.message ? err.message : err) }
  }
}

module.exports = { listApps, refreshApps, getIcon, listWindows, focusWindow }
