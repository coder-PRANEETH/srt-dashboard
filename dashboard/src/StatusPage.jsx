import { useMemo, useState } from 'react'
import { useNow, useTelemetry } from './useTelemetry'
import { ageText, decodeFrame, diagnosticsModel, filterFrames, freshFeed, hex, NODE_IDS, NODE_NAMES, parseId } from './diagnostics'

const EMPTY = []
export default function StatusPage() {
  const { feeds, events } = useTelemetry(true), now = useNow()
  const [selected, setSelected] = useState(5), [filters, setFilters] = useState({ id: '', node: '', query: '', issues: false })
  const [snapshot, setSnapshot] = useState(null), [notice, setNotice] = useState('')
  const model = diagnosticsModel(feeds, now), node = model.nodes.find(n => n.id === selected) || model.nodes[3]
  const source = snapshot || freshFeed(feeds.log, now)
  const raw = source?.frames || EMPTY
  const frames = useMemo(() => raw.slice(-64).map(decodeFrame).reverse(), [raw])
  const visible = useMemo(() => filterFrames(frames, filters), [frames, filters])
  const invalidId = parseId(filters.id) === null
  const change = key => e => setFilters(previous => ({ ...previous, [key]: key === 'issues' ? e.target.checked : e.target.value }))
  const jump = id => document.getElementById(id)?.scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'instant' : 'smooth', block: 'start' })
  const inspect = () => { setSelected(model.faults[0]?.node || 5); jump('node-detail') }
  const exportText = () => JSON.stringify({ capturedAt: new Date(snapshot?.capturedAt || feeds.log?.at || now).toISOString(), paused: !!snapshot, uptimeMs: source?.uptimeMs, total: source?.total, filters, frames: visible, diagnostics: freshFeed(feeds.diagnostics, now), events }, null, 2)
  const copy = async () => {
    try { await navigator.clipboard.writeText(exportText()); setNotice('Copied') }
    catch {
      // Clipboard API is unavailable on plain HTTP ESP32 access points.
      const area = document.createElement('textarea')
      area.value = exportText(); area.style.cssText = 'position:fixed;left:-9999px;top:0'
      document.body.append(area); area.select()
      let copied = false
      try { copied = document.execCommand('copy') } catch { /* Export remains available. */ }
      area.remove(); setNotice(copied ? 'Copied' : 'Copy unavailable · use Export')
    }
  }
  const download = () => {
    const url = URL.createObjectURL(new Blob([exportText()], { type: 'application/json' }))
    const a = document.createElement('a'); a.href = url; a.download = `srt-can-${Date.now()}.json`; a.click()
    setTimeout(() => URL.revokeObjectURL(url), 1000); setNotice(`Exported ${visible.length} frames`)
  }
  return <div className="page">
    <header className="topbar"><span className="wordmark">SRT</span><a className="button" href="/">← DASHBOARD</a></header>
    <main className="status-main"><div className="title-row"><h1>Status</h1><div className="connection"><span className={`pill ${model.connected ? '' : 'attention'}`}><i className="dot" />{model.connected ? 'CONNECTED' : 'DISCONNECTED'}</span><span className="pill">CAN {model.canReady === true ? 'READY' : model.canReady === false ? 'FAULT' : 'UNKNOWN'}</span><a className="button" href="#can-log">CAN LOG ↓</a></div></div>
      {model.faults.length > 0 && <div className="issue-banner" role="status"><span className="issue-icon" aria-hidden="true">!</span><div><strong>{model.faults.length} issues · {model.faults[0].label}</strong></div><button className="button" onClick={inspect}>INSPECT ↗</button></div>}
      <div className="section-head"><h2>Nodes</h2></div><div className="nodes">{model.nodes.map(n => {
        const sensorIssue = n.sensors.some(s => ['UNAVAILABLE', 'FAULT'].includes(s.state))
        return <button key={n.id} className={`node ${['OFFLINE', 'FAULT'].includes(n.state) || sensorIssue ? 'fault' : ''}`} aria-pressed={n.id === selected} aria-controls="node-detail" onClick={() => setSelected(n.id)}>
          <span className="node-top"><span>NODE {String(n.id).padStart(2, '0')}</span><span aria-hidden="true">{sensorIssue || ['OFFLINE', 'FAULT'].includes(n.state) ? '!' : '↗'}</span></span><span className="node-title">{n.name}</span><span className="node-state">{n.state}{sensorIssue && ['LIVE', 'LOCAL'].includes(n.state) ? ' · SENSOR !' : ''}</span><span className="node-note">{n.age}</span>
        </button>
      })}</div>
      <section className="node-detail" id="node-detail" aria-label={`Node ${node.id} sensors`}><div className="detail-head"><div><div className="caps muted">NODE {node.id}</div><h2>{node.name}</h2></div><button className="button" onClick={() => { setFilters({ id: '', node: String(selected), query: '', issues: false }); jump('can-log') }}>FRAMES ↓</button></div>
        <div className="sensor-grid">{node.sensors.map(sensor => <div key={sensor.name} className={`sensor ${['UNAVAILABLE', 'FAULT'].includes(sensor.state) ? 'fault' : ''}`}><div className="sensor-title"><span>{sensor.name}</span><span className="sensor-state">{['UNAVAILABLE', 'FAULT'].includes(sensor.state) ? '! ' : ''}{sensor.state}</span></div><strong>{sensor.value}</strong>{sensor.detail && <small>{sensor.detail}</small>}</div>)}</div>
      </section>
      <section id="can-log" className="log-section" aria-labelledby="log-title"><div className="section-head"><div className="log-title"><h2 id="log-title">CAN log</h2><span className="pill live-pill">{snapshot ? 'Ⅱ PAUSED' : source ? '● LIVE' : 'UNAVAILABLE'}</span></div><div className="log-actions"><button className="button" disabled={!snapshot && !source} onClick={() => { setSnapshot(snapshot ? null : { ...source, capturedAt: feeds.log.at }); setNotice('') }}>{snapshot ? '▶ RESUME' : 'Ⅱ PAUSE'}</button><button className="button" disabled={!source} onClick={copy}>COPY</button><button className="button" disabled={!source} onClick={download}>EXPORT ↓</button></div></div>
        <div className="filters"><div className="field"><label htmlFor="can-id">CAN ID</label><input id="can-id" value={filters.id} onChange={change('id')} autoComplete="off" spellCheck="false" placeholder="0x301" aria-invalid={invalidId} aria-describedby={invalidId ? 'log-empty' : undefined} /></div>
          <div className="field"><label htmlFor="node-filter">NODE</label><select id="node-filter" value={filters.node} onChange={change('node')}><option value="">All nodes</option>{NODE_NAMES.map((name, n) => <option key={name} value={NODE_IDS[n]}>{NODE_IDS[n]} · {name}</option>)}</select></div>
          <div className="field search-field"><label htmlFor="search">SEARCH</label><input id="search" value={filters.query} onChange={change('query')} placeholder="Message or bytes" autoComplete="off" /></div>
          <div className="filter-extras"><label><input type="checkbox" checked={filters.issues} onChange={change('issues')} />Issues only</label><button className="button" onClick={() => { setFilters({ id: '', node: '', query: '', issues: false }); setNotice('') }}>RESET</button></div>
        </div><div className="log-meta"><span>{visible.length} / {frames.length} frames · {source?.total ?? '—'} total</span><span>64-FRAME BUFFER</span></div>
        <p className="notice" role="status">{notice}</p><div className="log-list">{visible.length ? visible.map(f => <article key={`${f.seq}-${f.ms}`} className={`message ${f.issue ? 'issue' : ''}`}><div className="message-labels"><div className="message-id mono">{hex(f.id)}</div><div className="message-source">{f.node ? `NODE ${f.node}` : 'UNKNOWN'} · {f.issue ? '! ISSUE' : f.tx ? 'TRANSMITTED' : 'RECEIVED'}</div></div><div className="message-description"><strong>{f.kind}</strong>{f.text}</div><time className="message-time mono">{source.uptimeMs === undefined ? `${(f.ms/1000).toFixed(2)} s uptime` : `${ageText((source.uptimeMs - f.ms) >>> 0)} ago`}</time><details><summary>HEX · DLC {f.dlc}</summary><div className="raw mono">{f.bytes || '∅'} · #{f.seq}</div></details></article>) : <div className="empty" id="log-empty">{invalidId ? 'Invalid ID · use 0–1FFFFFFF' : !source ? 'CAN log unavailable' : 'No matches'}</div>}</div>
      </section>
      <section className="recent"><div className="section-head"><h2>Events</h2><span className="caps">Last 24</span></div>{events.length ? events.map((event, i) => <div className="event" key={`${event.at}-${event.key}-${i}`}><time className="mono">{new Date(event.at).toLocaleTimeString([], { hour12: false })}</time><span>{event.recovered ? '✓ Recovered · ' : '! '}{event.label}</span></div>) : <div className="muted">—</div>}</section>
    </main>
  </div>
}
