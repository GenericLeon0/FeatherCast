// Lightweight fuzzy search: case-insensitive subsequence match with score.
// Bonus for matches at word boundaries and consecutive matches.
function score(query, target) {
  const q = query.toLowerCase()
  const t = target.toLowerCase()
  if (q.length === 0) return 1
  if (t.includes(q)) {
    // direct substring – highest score, bonus if at start
    return t.startsWith(q) ? 1000 : 500 - t.indexOf(q)
  }

  let qi = 0
  let s = 0
  let prevMatchIdx = -2
  for (let ti = 0; ti < t.length && qi < q.length; ti++) {
    if (t[ti] === q[qi]) {
      s += 10
      if (ti === prevMatchIdx + 1) s += 8 // consecutive
      if (ti === 0 || t[ti - 1] === ' ' || t[ti - 1] === '-' || t[ti - 1] === '_') s += 6
      prevMatchIdx = ti
      qi++
    }
  }
  if (qi < q.length) return -1 // not all characters found
  return s
}

export function search(query, items) {
  if (!query || !query.trim()) return items
  const scored = []
  for (const item of items) {
    const sc = score(query, item.name)
    if (sc >= 0) scored.push({ item, sc })
  }
  scored.sort((a, b) => b.sc - a.sc || a.item.name.localeCompare(b.item.name))
  return scored.map((x) => x.item)
}
