import React, { useEffect, useState, useCallback } from 'react'

const api = window.leancast

const MODIFIER_KEYS = ['Control', 'Alt', 'Shift', 'Meta']

// Browser KeyboardEvent → Electron Accelerator (e.g. "Alt+Space").
function toAccelerator(e) {
  const mods = []
  if (e.ctrlKey) mods.push('Control')
  if (e.altKey) mods.push('Alt')
  if (e.shiftKey) mods.push('Shift')
  if (e.metaKey) mods.push('Super')

  let key = null
  const code = e.code
  if (code === 'Space') key = 'Space'
  else if (/^Key([A-Z])$/.test(code)) key = code.slice(3)
  else if (/^Digit([0-9])$/.test(code)) key = code.slice(5)
  else if (/^F([0-9]{1,2})$/.test(code)) key = code
  else if (code === 'Enter') key = 'Return'
  else if (code === 'Tab') key = 'Tab'
  else if (code === 'Backquote') key = '`'
  else if (e.key.length === 1) key = e.key.toUpperCase()

  if (!key || MODIFIER_KEYS.includes(e.key)) return null
  if (mods.length === 0) return null // at least one modifier required
  return [...mods, key].join('+')
}

export default function Settings({ onClose }) {
  const [shortcut, setShortcut] = useState('')
  const [recording, setRecording] = useState(false)
  const [pending, setPending] = useState(null)
  const [error, setError] = useState('')
  const [saved, setSaved] = useState(false)
  const [compact, setCompact] = useState(false)

  useEffect(() => {
    api.getSettings().then((s) => {
      setShortcut(s.shortcut)
      setCompact(!!s.compactMode)
    })
  }, [])

  const toggleCompact = useCallback(() => {
    const next = !compact
    setCompact(next)
    api.setCompact(next)
  }, [compact])

  const onRecordKeyDown = useCallback(
    (e) => {
      if (!recording) return
      e.preventDefault()
      e.stopPropagation()
      if (e.key === 'Escape') {
        setRecording(false)
        setPending(null)
        return
      }
      const acc = toAccelerator(e)
      if (acc) {
        setPending(acc)
        setRecording(false)
      }
    },
    [recording]
  )

  const save = useCallback(async () => {
    if (!pending) return
    setError('')
    setSaved(false)
    const res = await api.setShortcut(pending)
    if (res.ok) {
      setShortcut(pending)
      setPending(null)
      setSaved(true)
    } else {
      setError(res.error || 'Unknown error.')
    }
  }, [pending])

  return (
    <div className="settings">
      <div className="settings-header">
        <button className="back-btn" onClick={onClose} title="Back">←</button>
        <h2>Settings</h2>
      </div>

      <div className="settings-body">
        <label className="setting-label">Global Shortcut</label>
        <p className="setting-desc">
          Keyboard shortcut to open LeanCast. Current:&nbsp;
          <span className="kbd">{shortcut || '–'}</span>
        </p>

        <div className="shortcut-row">
          <button
            className={'record-btn' + (recording ? ' recording' : '')}
            onClick={() => {
              setRecording(true)
              setPending(null)
              setError('')
              setSaved(false)
            }}
            onKeyDown={onRecordKeyDown}
          >
            {recording
              ? 'Press a key…'
              : pending
              ? pending
              : 'Record new shortcut'}
          </button>

          <button className="save-btn" disabled={!pending} onClick={save}>
            Save
          </button>
        </div>

        {error && <div className="settings-error">{error}</div>}
        {saved && <div className="settings-ok">Saved ✓</div>}

        <p className="setting-hint">
          At least one modifier (Ctrl/Alt/Shift) is required. Example:
          <span className="kbd">Alt+Space</span>, <span className="kbd">Control+Space</span>.
        </p>

        <label className="setting-label setting-label-spaced">Compact Mode</label>
        <p className="setting-desc">
          Shows only the search bar at rest; results expand below.
        </p>
        <button
          className={'toggle' + (compact ? ' on' : '')}
          role="switch"
          aria-checked={compact}
          onClick={toggleCompact}
        >
          <span className="toggle-knob" />
          <span className="toggle-text">{compact ? 'On' : 'Off'}</span>
        </button>
      </div>
    </div>
  )
}
