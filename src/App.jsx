import React, { useEffect, useMemo, useRef, useState, useCallback } from 'react'
import { search } from './fuzzy.js'
import Settings from './Settings.jsx'

const api = window.leancast

// Icon source for an entry: app → .lnk path, window → exe path.
function iconKey(item) {
  return item.kind === 'window' ? item.exe : item.path
}

// Unique React key (windows don't have a unique path).
function itemKey(item) {
  return item.kind === 'window' ? `win:${item.pid}` : item.path
}

export default function App() {
  const [apps, setApps] = useState([])
  const [windows, setWindows] = useState([])
  const [recentPaths, setRecentPaths] = useState([])
  const [compact, setCompact] = useState(false)
  const [query, setQuery] = useState('')
  const [selected, setSelected] = useState(0)
  const [view, setView] = useState('search')
  const [icons, setIcons] = useState({}) // iconKey -> dataURL
  const inputRef = useRef(null)
  const listRef = useRef(null)
  const windowRef = useRef(null)

  // Load apps once on mount.
  useEffect(() => {
    api.listApps().then((list) => setApps(list || []))
  }, [])

  // Reload settings + open windows on each show.
  const refreshState = useCallback(() => {
    api.getSettings().then((s) => {
      setRecentPaths(s.recentApps || [])
      setCompact(!!s.compactMode)
    })
    api.listWindows().then((w) => setWindows(w || []))
  }, [])

  useEffect(() => {
    refreshState()
  }, [refreshState])

  // React to show events from the main process (hotkey or tray).
  useEffect(() => {
    const offShow = api.onShow((nextView) => {
      setQuery('')
      setSelected(0)
      setView(nextView || 'search')
      refreshState()
      requestAnimationFrame(() => inputRef.current && inputRef.current.focus())
    })
    return () => {
      offShow && offShow()
    }
  }, [refreshState])

  // Searchable pool: open windows first, then apps.
  const pool = useMemo(() => [...windows, ...apps], [windows, apps])

  // Recently used apps: map stored paths to existing app entries.
  const recentApps = useMemo(() => {
    const byPath = new Map(apps.map((a) => [a.path, a]))
    return recentPaths.map((p) => byPath.get(p)).filter(Boolean)
  }, [recentPaths, apps])

  const isEmpty = query.trim() === ''

  // Display list: empty query → recent + windows (with sections); otherwise search hits.
  const displayItems = useMemo(() => {
    if (isEmpty) return [...recentApps, ...windows]
    return search(query, pool)
  }, [isEmpty, recentApps, windows, query, pool])

  // Reset selection when the list changes.
  useEffect(() => {
    setSelected(0)
  }, [query, displayItems.length])

  // Lazy-load icons for visible results.
  useEffect(() => {
    const visible = displayItems.slice(0, 60)
    visible.forEach((item) => {
      const key = iconKey(item)
      if (!key) return
      if (icons[key] === undefined) {
        setIcons((prev) => ({ ...prev, [key]: null })) // mark as "in progress"
        api.getIcon(key).then((dataUrl) => {
          setIcons((prev) => ({ ...prev, [key]: dataUrl }))
        })
      }
    })
  }, [displayItems]) // eslint-disable-line react-hooks/exhaustive-deps

  // Scroll the selected item into view.
  useEffect(() => {
    const el = listRef.current && listRef.current.querySelector('[data-selected="true"]')
    if (el) el.scrollIntoView({ block: 'nearest' })
  }, [selected])

  // In compact mode, adjust window height to match content.
  useEffect(() => {
    if (!compact || view !== 'search') return
    const el = windowRef.current
    if (!el || typeof ResizeObserver === 'undefined') return
    const report = () => api.setWindowHeight(el.offsetHeight)
    report()
    const ro = new ResizeObserver(report)
    ro.observe(el)
    return () => ro.disconnect()
  }, [compact, view])

  const activate = useCallback((item) => {
    if (!item) return
    if (item.kind === 'window') api.focusWindow(item.hwnd)
    else api.launch(item.path)
  }, [])

  const onKeyDown = useCallback(
    (e) => {
      if (e.key === 'Escape') {
        e.preventDefault()
        if (view === 'settings') setView('search')
        else api.hide()
        return
      }
      if (view !== 'search') return
      if (e.key === 'ArrowDown') {
        e.preventDefault()
        setSelected((i) => Math.min(i + 1, displayItems.length - 1))
      } else if (e.key === 'ArrowUp') {
        e.preventDefault()
        setSelected((i) => Math.max(i - 1, 0))
      } else if (e.key === 'Enter') {
        e.preventDefault()
        activate(displayItems[selected])
      }
    },
    [view, displayItems, selected, activate]
  )

  if (view === 'settings') {
    return (
      <div className="window" ref={windowRef} onKeyDown={onKeyDown}>
        <Settings onClose={() => setView('search')} />
      </div>
    )
  }

  // Render a result row; index = sequential selection index.
  const renderRow = (item, index) => {
    const key = iconKey(item)
    const url = key ? icons[key] : null
    const isSel = index === selected
    return (
      <div
        key={itemKey(item)}
        className={'result-row' + (isSel ? ' selected' : '')}
        data-selected={isSel}
        onMouseMove={() => setSelected(index)}
        onClick={() => activate(item)}
      >
        <div className="result-icon">
          {url ? (
            <img src={url} alt="" />
          ) : (
            <div className="icon-placeholder">{item.name[0]?.toUpperCase()}</div>
          )}
        </div>
        <div className="result-name">{item.name}</div>
        {isSel && (
          <div className="result-hint">{item.kind === 'window' ? '↵ Switch' : '↵ Open'}</div>
        )}
      </div>
    )
  }

  return (
    <div className={'window' + (compact ? ' compact' : '')} ref={windowRef} onKeyDown={onKeyDown}>
      <div className="search-bar">
        <span className="search-icon">⌕</span>
        <input
          ref={inputRef}
          className="search-input"
          type="text"
          placeholder="Search apps or windows…"
          value={query}
          autoFocus
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
        />
        <button
          className="gear-btn"
          title="Settings"
          onClick={() => setView('settings')}
        >
          ⚙
        </button>
      </div>

      <div className="results" ref={listRef}>
        {isEmpty ? (
          <>
            {recentApps.length === 0 && windows.length === 0 && (
              <div className="empty">Type to search for an app or window</div>
            )}
            {recentApps.length > 0 && (
              <>
                <div className="section-label">Recently used</div>
                {recentApps.map((item, i) => renderRow(item, i))}
              </>
            )}
            {windows.length > 0 && (
              <>
                <div className="section-label">Open windows</div>
                {windows.map((item, i) => renderRow(item, recentApps.length + i))}
              </>
            )}
          </>
        ) : (
          <>
            {displayItems.length === 0 && <div className="empty">No results</div>}
            {displayItems.slice(0, 200).map((item, i) => renderRow(item, i))}
          </>
        )}
      </div>

      <div className="footer">
        <span className="brand">LeanCast</span>
        <span className="footer-hint">↑↓ Navigate · ↵ Open · Esc Close</span>
      </div>
    </div>
  )
}
