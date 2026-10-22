import { useCallback, useEffect, useState } from 'react'
import MapView from './MapView'
import { advanceTrack, useNow, useTelemetry, validFix } from './useTelemetry'
import { display, freshFeed, number } from './diagnostics'

function introNeeded() {
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return false
  try { return !sessionStorage.getItem('srt-intro') } catch { return true }
}
function Wheel({ angle, available }) {
  return <div className={`wheel-figure ${available ? '' : 'unavailable'}`}><svg viewBox="0 0 320 320" role="img" aria-label={available ? `Steering angle ${angle} degrees` : 'Steering angle unavailable'}>
    <circle cx="160" cy="160" r="152" fill="none" stroke="rgba(255,255,255,.25)" strokeDasharray="1 10" />
    <path d="M53 53A151 151 0 0 1 267 53M160 2v12M160 306v12M2 160h12M306 160h12" fill="none" stroke="#fff" strokeWidth="2" />
    <g className="wheel-rotor" style={{ '--steering-angle': `${angle}deg` }}>
      <circle cx="160" cy="160" r="121" fill="#120b06" stroke="rgba(255,255,255,.25)" strokeWidth="20" />
      <circle cx="160" cy="160" r="121" fill="none" stroke="#fff" strokeWidth="2" />
      <path d="M152 39h16v17h-16z" fill="#ff8a24" />
      <path d="M51 125 131 144 144 172 67 190M269 125 189 144 176 172 253 190M144 188 140 271 180 271 176 188" fill="#211208" stroke="rgba(255,255,255,.6)" strokeWidth="1.5" />
      <circle cx="160" cy="160" r="39" fill="#120b06" stroke="#fff" strokeWidth="1.5" />
      <path d="M141 160h38M160 141v38" stroke="rgba(255,255,255,.25)" /><circle cx="160" cy="160" r="8" fill="none" stroke="#fff" strokeWidth="2" />
    </g></svg></div>
}
export default function HomePage() {
  const { feeds } = useTelemetry(), now = useNow()
  const [intro, setIntro] = useState(introNeeded), [mapOpen, setMapOpen] = useState(false)
  const closeMap = useCallback(() => setMapOpen(false), [])
  const [memory, setMemory] = useState({ gps: null, vehicle: null, angle: 0, track: { points: [], trip: 0, heading: null, at: 0 } })
  if (memory.gps !== feeds.gps || memory.vehicle !== feeds.vehicle) {
    const v = feeds.vehicle?.data
    setMemory({ gps: feeds.gps, vehicle: feeds.vehicle, angle: v?.steeringLink && number(v.steeringDeg) && Math.abs(v.steeringDeg) <= 180 ? v.steeringDeg : memory.angle,
      track: memory.gps !== feeds.gps ? advanceTrack(memory.track, feeds.gps?.data, feeds.gps?.at) : memory.track })
  }
  useEffect(() => {
    try { sessionStorage.setItem('srt-intro', '1') } catch { /* Storage is optional on private browsers. */ }
    const timer = setTimeout(() => setIntro(false), 1400)
    return () => clearTimeout(timer)
  }, [])
  const vehicle = freshFeed(feeds.vehicle, now), gps = freshFeed(feeds.gps, now)
  const fix = validFix(gps) && (gps.ageMs || 0) + now - feeds.gps.at < 3000
  const steeringAvailable = !!vehicle?.steeringLink && number(vehicle.steeringDeg) && Math.abs(vehicle.steeringDeg) <= 180 && now - feeds.vehicle.at < 1000
  const accelerator = now - (feeds.vehicle?.at || 0) < 750 ? vehicle?.acceleratorPct : null
  const speed = fix && number(gps.speed) ? Math.max(0, Math.min(160, gps.speed)) : null
  const digits = number(speed) ? String(Math.round(speed)).padStart(3, '0') : '—'
  const leading = number(speed) ? digits.match(/^0{1,2}(?=\d)/)?.[0] || '' : ''
  const { track } = memory
  return <>
    <div className="cockpit" inert={mapOpen || intro}>
      <header className="topbar"><span className="wordmark">SRT</span><div className="top-actions"><span className={`pill ${fix ? '' : 'attention'}`}><i className="dot" />{fix ? 'GPS FIX' : 'NO FIX'}</span><time className="mono">{new Date(now).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false })}</time></div></header>
      <main className="drive"><div className="instruments">
        <section aria-label="GPS speed"><div className="section-label caps">GPS speed</div><div className="speed-value"><span className="ghost">{leading}</span>{digits.slice(leading.length)}</div><div className="speed-unit">KM / H</div><div className="speed-rule" style={{ '--speed': `${(speed || 0) / 1.6}%` }} /><div className="speed-ticks mono">{[0, 40, 80, 120, 160].map(n => <span key={n}>{n}</span>)}</div></section>
        <section aria-label="Steering angle" className="wheel-module"><div className="wheel-caption caps">STEERING ANGLE</div><Wheel angle={memory.angle} available={steeringAvailable} /><div className="wheel-reading"><strong className="mono">{steeringAvailable ? `${vehicle.steeringDeg > 0 ? '+' : ''}${vehicle.steeringDeg.toFixed(1)}°` : '—'}</strong></div>{!steeringAvailable && <span className="sensor-state">UNAVAILABLE</span>}</section>
        <section aria-label="Accelerator input" className="accelerator"><div className="section-label caps">Accelerator</div><div className="accel-reading mono">{display(accelerator)}<small>%</small></div><div className="pedal-bars" aria-hidden="true">{Array.from({ length: 10 }, (_, n) => <i key={n} className={`pedal-bar ${number(accelerator) && accelerator > n * 10 ? 'on' : ''}`} style={{ height: `${25 + n * 7.5}%` }} />)}</div></section>
      </div><div className="bottom-rail">
        <div><div className="caps muted">TRIP</div><div className="rail-value mono">{track.trip.toFixed(2)}<small>km</small></div></div>
        <div><div className="caps muted">Heading</div><div className="rail-value mono">{fix ? display(track.heading) : '—'}<small>°</small></div></div>
        <div><div className="caps muted">SATELLITES</div><div className="rail-value mono">{fix ? display(gps.sats) : '—'}</div></div>
        <div className="drive-actions"><button className="button primary" onClick={() => setMapOpen(true)}><svg className="stroke-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="m3 5 6-2 6 2 6-2v16l-6 2-6-2-6 2V5ZM9 3v16M15 5v16" /></svg>MAP ↗</button></div>
      </div></main>
    </div>
    {intro && <button className="launch" aria-label="Skip SRT opening animation" onClick={() => setIntro(false)} autoFocus><span className="wordmark">SRT</span><span className="launch-line" /></button>}
    {mapOpen && <MapView points={track.points} fix={fix} onClose={closeMap} />}
  </>
}
