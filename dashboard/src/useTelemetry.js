import { useEffect, useReducer, useState } from 'react'
import { diagnosticsModel, number } from './diagnostics.js'

export function useNow() {
  const [now, setNow] = useState(Date.now)
  useEffect(() => { const timer = setInterval(() => setNow(Date.now()), 250); return () => clearInterval(timer) }, [])
  return now
}
const HOME = ['gps', 'vehicle'], STATUS = ['vehicle', 'gps', 'diagnostics', 'log']
function reduceFeed(previous, action) {
  const feeds = action.feeds
  if (!action.status) return { feeds, events: [] }
  const faults = diagnosticsModel(feeds, action.at).faults
  const old = previous.faults || []
  const events = [...faults.filter(f => !old.some(o => o.key === f.key)).map(f => ({ ...f, at: action.at, recovered: false })), ...old.filter(o => !faults.some(f => f.key === o.key)).map(f => ({ ...f, at: action.at, recovered: true }))]
  const before = previous.feeds?.diagnostics?.data, next = feeds.diagnostics?.data
  if (before && next) {
    if (next.uptimeMs < before.uptimeMs && before.uptimeMs < 0xffff0000) events.unshift({ key: 'restart', label: 'Host restarted', at: action.at })
    for (const [key, label] of [['txFailed', 'CAN TX failures'], ['malformedFrames', 'Malformed frames']]) if (next[key] > before[key]) events.unshift({ key, label: `${label} +${next[key] - before[key]}`, at: action.at })
  }
  return { feeds, faults, events: [...events, ...previous.events].slice(0, 24) }
}
export function useTelemetry(status = false) {
  const [state, dispatch] = useReducer(reduceFeed, { feeds: {}, events: [] })
  useEffect(() => {
    const lifetime = new AbortController()
    let timer, wake
    async function poll() {
      while (!lifetime.signal.aborted) {
        const feeds = {}
        for (const key of status ? STATUS : HOME) {
          if (lifetime.signal.aborted) return
          const request = new AbortController()
          const abort = () => request.abort()
          lifetime.signal.addEventListener('abort', abort, { once: true })
          const timeout = setTimeout(abort, 2500)
          try {
            const response = await fetch(`/api/${key === 'log' ? 'can-log' : key}`, { cache: 'no-store', signal: request.signal })
            if (!response.ok) throw new Error(`HTTP ${response.status}`)
            const data = await response.json()
            if (!data || typeof data !== 'object' || Array.isArray(data)) throw new Error('Invalid response')
            feeds[key] = { data, at: Date.now(), error: null }
          } catch (error) { feeds[key] = { data: null, at: Date.now(), error: error.message } }
          finally { clearTimeout(timeout); lifetime.signal.removeEventListener('abort', abort) }
        }
        if (lifetime.signal.aborted) return
        dispatch({ feeds, at: Date.now(), status })
        await new Promise(resolve => { wake = resolve; timer = setTimeout(resolve, document.hidden ? 4000 : status ? 750 : 400) })
      }
    }
    poll()
    return () => { lifetime.abort(); clearTimeout(timer); wake?.() }
  }, [status])
  return state
}
export const validFix = g => !!g?.fix && number(g.lat) && number(g.lon) && Math.abs(g.lat) <= 90 && Math.abs(g.lon) <= 180 && (Math.abs(g.lat) > .0001 || Math.abs(g.lon) > .0001)
export function distanceKm(a, b) {
  const rad = Math.PI / 180, dx = (b.lat-a.lat)*rad, dy = (b.lon-a.lon)*rad
  const h = Math.sin(dx/2)**2 + Math.cos(a.lat*rad)*Math.cos(b.lat*rad)*Math.sin(dy/2)**2
  return 12742 * Math.asin(Math.min(1, Math.sqrt(h)))
}
export function bearing(a, b) {
  const rad = Math.PI / 180, y = Math.sin((b.lon-a.lon)*rad)*Math.cos(b.lat*rad)
  const x = Math.cos(a.lat*rad)*Math.sin(b.lat*rad)-Math.sin(a.lat*rad)*Math.cos(b.lat*rad)*Math.cos((b.lon-a.lon)*rad)
  return (Math.atan2(y,x)/rad+360)%360
}
export function advanceTrack(previous, gps, receivedAt) {
  if (!validFix(gps)) return previous
  const at = receivedAt - (number(gps.ageMs) ? gps.ageMs : 0)
  if (at <= previous.at) return previous
  const point = { lat: gps.lat, lon: gps.lon }, last = previous.points.at(-1)
  if (!last) return { points: [point], trip: 0, heading: null, at }
  const km = distanceKm(last, point), seconds = Math.max(.5, (at - previous.at)/1000)
  // Reject GPS jumps; don't accumulate stationary jitter or bridge a long outage.
  if (km > 2 || km > (15 + 63 * seconds)/1000) return previous
  if (km < .003) return { ...previous, at }
  const gap = seconds >= 10
  return { points: [...previous.points, { ...point, gap }].slice(-600), trip: previous.trip + (gap ? 0 : km), heading: gap ? previous.heading : bearing(last, point), at }
}
