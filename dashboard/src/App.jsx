import { useEffect, useMemo, useRef, useState } from 'react'
import L from 'leaflet'
import 'leaflet/dist/leaflet.css'
import './App.css'

const MAX_SPEED = 160

// Route origin for the live map trail (lat, lon).
const START_POS = { lat: 12.9716, lon: 77.5946 }

const initialTelemetry = {
  speed: 27,
  battery: 78,
  range: 248,
  voltage: 382.4,
  current: 84.2,
  batteryTemp: 31.4,
  motorTemp: 54.8,
  controllerTemp: 42.1,
  ambientTemp: 22.7,
  rpm: 3240,
  power: 31.8,
  regen: 0,
  tripDistance: 36.4,
  signalStrength: 92,
  latency: 18,
  heading: 78,
}

const driveModes = ['Eco', 'Normal', 'Sport']

/* ---------- animation helpers ---------- */

// Smoothly eases a displayed number toward its live target using rAF.
function useAnimatedNumber(target, speed = 0.14) {
  const [display, setDisplay] = useState(target)
  const frame = useRef()
  const value = useRef(target)

  useEffect(() => {
    const step = () => {
      const diff = target - value.current
      if (Math.abs(diff) < 0.01) {
        value.current = target
        setDisplay(target)
        return
      }
      value.current += diff * speed
      setDisplay(value.current)
      frame.current = requestAnimationFrame(step)
    }
    frame.current = requestAnimationFrame(step)
    return () => cancelAnimationFrame(frame.current)
  }, [target, speed])

  return display
}

function Icon({ name, size = 16 }) {
  const paths = {
    bolt: <path d="m13 2-9 12h7l-1 8 9-12h-7l1-8Z" />,
    battery: <><rect x="2" y="7" width="17" height="10" rx="2.5" /><path d="M22 11v2" /></>,
    wifi: <><path d="M5 12.5a11 11 0 0 1 14 0M8 16a6.5 6.5 0 0 1 8 0" /><circle cx="12" cy="19.5" r="1" /></>,
    gauge: <><path d="M4.9 17a8 8 0 1 1 14.2 0" /><path d="m12 13 3-3" /></>,
    temp: <><path d="M14 14.8V4a2 2 0 1 0-4 0v10.8a4 4 0 1 0 4 0Z" /></>,
    road: <><path d="M4 20 8 4M20 20 16 4M12 5v3M12 11v3M12 17v3" /></>,
    alert: <><path d="M12 3 2 20h20L12 3Z" /><path d="M12 10v4M12 17.5v.5" /></>,
    camera: <><path d="M3 8.5A2.5 2.5 0 0 1 5.5 6h1.8l1.2-2h6.6l1.2 2h1.8A2.5 2.5 0 0 1 20.5 8.5v8A2.5 2.5 0 0 1 18 19H5.5A2.5 2.5 0 0 1 3 16.5Z" /><circle cx="11.75" cy="12" r="3.4" /></>,
    map: <><path d="M9 4 3 6.5v13L9 17l6 2.5 6-2.5v-13L15 6.5Z" /><path d="M9 4v13M15 6.5v13" /></>,
  }
  return (
    <svg className="icon" width={size} height={size} viewBox="0 0 24 24" fill="none"
      stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      {paths[name]}
    </svg>
  )
}

/* ---------- hero: speed ---------- */

function SpeedDial({ speed, driveMode, setDriveMode, rpm, power }) {
  const shown = useAnimatedNumber(speed, 0.18)
  const pct = Math.min(1, shown / MAX_SPEED)

  // 240° sweep starting at 150°
  const R = 118
  const ARC = (240 / 360) * 2 * Math.PI * R
  const ticks = Array.from({ length: 9 }, (_, i) => i * 20)

  return (
    <section className="hero-speed">
      <div className="dial">
        <svg viewBox="0 0 300 300" className="dial-svg">
          <defs>
            <linearGradient id="speedGrad" x1="0" y1="1" x2="1" y2="0">
              <stop offset="0%" stopColor="#0a7d3c" />
              <stop offset="55%" stopColor="#0d8ee0" />
              <stop offset="100%" stopColor="#d81f26" />
            </linearGradient>
          </defs>
          <circle className="dial-track" cx="150" cy="150" r={R}
            strokeDasharray={`${ARC} 999`} transform="rotate(150 150 150)" />
          <circle className="dial-fill" cx="150" cy="150" r={R}
            strokeDasharray={`${ARC * pct} 999`} transform="rotate(150 150 150)" />
          {ticks.map((t) => {
            const a = (150 + (t / MAX_SPEED) * 240) * (Math.PI / 180)
            const inner = t % 40 === 0 ? 94 : 101
            return (
              <line key={t} className={`dial-tick ${t % 40 === 0 ? 'major' : ''}`}
                x1={150 + Math.cos(a) * inner} y1={150 + Math.sin(a) * inner}
                x2={150 + Math.cos(a) * 107} y2={150 + Math.sin(a) * 107} />
            )
          })}
        </svg>
        <div className="dial-center">
          <strong className="speed-value">{Math.round(shown)}</strong>
          <span className="speed-unit">km/h</span>
        </div>
      </div>
      <div className="mode-switch">
        {driveModes.map((mode) => (
          <button key={mode}
            className={`mode-btn ${mode === driveMode ? 'active' : ''} m-${mode.toLowerCase()}`}
            onClick={() => setDriveMode(mode)}>
            {mode.toUpperCase()}
          </button>
        ))}
      </div>
      <div className="hero-sub">
        <div className="sub-stat"><span>RPM</span><strong>{Math.round(rpm).toLocaleString()}</strong></div>
        <div className="sub-stat"><span>POWER</span><strong>{power.toFixed(1)}<i>kW</i></strong></div>
      </div>
    </section>
  )
}

/* ---------- hero: battery ---------- */

function BatteryHero({ battery, range, voltage, current, power, charging }) {
  const shown = useAnimatedNumber(battery, 0.1)
  const level = shown > 45 ? 'good' : shown > 18 ? 'mid' : 'low'

  // 260-degree arc, opening at the bottom
  const R = 80
  const SWEEP = 260
  const ARC = (SWEEP / 360) * 2 * Math.PI * R
  const pct = Math.max(0, Math.min(100, shown)) / 100

  // 12 discrete cell segments around the arc
  const CELLS = 12
  const litCells = Math.round(pct * CELLS)

  return (
    <section className={`hero-battery level-${level}`}>
      <div className="batt-gauge">
        <svg viewBox="0 0 220 220" className="batt-svg">
          <circle className="batt-track" cx="110" cy="110" r={R}
            strokeDasharray={`${ARC} 999`} transform="rotate(140 110 110)" />
          <circle className="batt-arc" cx="110" cy="110" r={R}
            strokeDasharray={`${ARC * pct} 999`} transform="rotate(140 110 110)" />
          {Array.from({ length: CELLS }, (_, i) => {
            const a = (140 + ((i + 0.5) / CELLS) * SWEEP) * (Math.PI / 180)
            return (
              <circle key={i} className={`batt-cell ${i < litCells ? 'lit' : ''}`}
                cx={110 + Math.cos(a) * (R - 22)} cy={110 + Math.sin(a) * (R - 22)} r="3.6"
                style={{ animationDelay: `${i * 90}ms` }} />
            )
          })}
        </svg>
        <div className="batt-core">
          <div className="batt-pct">
            <strong>{shown.toFixed(0)}</strong><span>%</span>
          </div>
          <span className={`flow-badge ${charging ? 'charging' : 'draw'}`}>
            <Icon name="bolt" size={12} />
            {charging ? 'REGEN' : `${power.toFixed(0)} kW`}
          </span>
        </div>
      </div>

      <div className="batt-range">
        <span className="lbl">EST. RANGE</span>
        <strong>{Math.round(range)}<i>km</i></strong>
      </div>

      <div className="batt-mini">
        <div><span>PACK</span><strong>{voltage.toFixed(0)}<i>V</i></strong></div>
        <div><span>CURRENT</span><strong>{current.toFixed(0)}<i>A</i></strong></div>
      </div>
    </section>
  )
}

/* ---------- thermal bars ---------- */

function TempBar({ label, value, max, warn }) {
  const shown = useAnimatedNumber(value, 0.12)
  const pct = Math.min(100, (shown / max) * 100)
  const tone = shown >= warn ? 'hot' : shown >= warn - 12 ? 'warm' : 'cool'
  return (
    <div className="temp-row">
      <span className="temp-name">{label}</span>
      <div className="temp-track">
        <div className={`temp-fill ${tone}`} style={{ width: `${pct}%` }} />
      </div>
      <strong className={`temp-num ${tone}`}>{shown.toFixed(0)}°</strong>
    </div>
  )
}

/* ---------- camera ---------- */

function CameraView({ speed, heading, time }) {
  // Simulated forward-facing feed: road perspective with motion tied to speed.
  const dur = Math.max(0.35, 3.2 - (speed / MAX_SPEED) * 2.7)

  return (
    <section className="card camera">
      <h2>
        <Icon name="camera" size={13} />FORWARD CAM
        <span className="cam-live"><i />LIVE</span>
      </h2>
      <div className="cam-feed" style={{ '--dash-dur': `${dur}s` }}>
        <div className="cam-sky" />
        <div className="cam-ground" />
        <div className="cam-road">
          <div className="cam-lane" />
        </div>
        <div className="cam-scan" />
        <div className="cam-hud">
          <span className="cam-tag">CAM 01 / FRONT</span>
          <span className="cam-tag right">{time}</span>
        </div>
        <div className="cam-crosshair">
          <span className="cam-heading">{Math.round(heading)}°</span>
        </div>
      </div>
    </section>
  )
}

/* ---------- map ---------- */

function MapView({ track, heading, speed, distance }) {
  const holder = useRef(null)
  const map = useRef(null)
  const trail = useRef(null)
  const marker = useRef(null)

  const last = track[track.length - 1] ?? START_POS

  // create the map once
  useEffect(() => {
    if (map.current || !holder.current) return

    map.current = L.map(holder.current, {
      center: [START_POS.lat, START_POS.lon],
      zoom: 16,
      zoomControl: false,
      attributionControl: true,
      dragging: false,
      scrollWheelZoom: false,
      doubleClickZoom: false,
      touchZoom: false,
      keyboard: false,
    })

    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap',
    }).addTo(map.current)

    trail.current = L.polyline([], {
      color: '#0d6fd8',
      weight: 4,
      opacity: 0.9,
      lineJoin: 'round',
    }).addTo(map.current)

    marker.current = L.marker([START_POS.lat, START_POS.lon], {
      icon: L.divIcon({
        className: 'map-marker',
        html: '<div class="map-marker-ping"></div><div class="map-marker-arrow"></div>',
        iconSize: [26, 26],
        iconAnchor: [13, 13],
      }),
      interactive: false,
    }).addTo(map.current)

    // the panel is grid-sized, so the container can still be 0px on first
    // paint - keep Leaflet's viewport in sync as the layout settles/resizes
    const ro = new ResizeObserver(() => map.current?.invalidateSize())
    ro.observe(holder.current)

    return () => {
      ro.disconnect()
      map.current?.remove()
      map.current = null
    }
  }, [])

  // follow the vehicle
  useEffect(() => {
    if (!map.current) return
    const latlngs = track.map((p) => [p.lat, p.lon])
    trail.current?.setLatLngs(latlngs)
    marker.current?.setLatLng([last.lat, last.lon])
    map.current.panTo([last.lat, last.lon], { animate: true, duration: 0.9 })

    const arrow = marker.current?.getElement()?.querySelector('.map-marker-arrow')
    if (arrow) arrow.style.transform = `rotate(${heading}deg)`
  }, [track, last.lat, last.lon, heading])

  return (
    <section className="card mapcard">
      <h2>
        <Icon name="map" size={13} />ROUTE
        <span className="map-dist">{distance.toFixed(1)} km</span>
      </h2>
      <div className="map-face">
        <div ref={holder} className="map-canvas" />
        <div className="map-speed">{Math.round(speed)}<i>km/h</i></div>
      </div>
    </section>
  )
}

/* ---------- app ---------- */

function App() {
  const [telemetry, setTelemetry] = useState(initialTelemetry)
  const [now, setNow] = useState(new Date())
  const [driveMode, setDriveMode] = useState('Normal')
  const [track, setTrack] = useState([START_POS])

  useEffect(() => {
    const timer = setInterval(() => {
      const tick = new Date()
      setNow(tick)
      setTelemetry((current) => {
        const modeFactor = driveMode === 'Eco' ? 0.78 : driveMode === 'Sport' ? 1.3 : 1
        const speed = Math.max(0, Math.min(140, current.speed + (Math.random() - 0.48) * (5 + modeFactor * 1.4)))
        const currentDraw = (75 + Math.random() * 16) * modeFactor
        const batteryDrop = 0.005 * modeFactor
        const next = {
          ...current,
          speed: Math.round(speed),
          current: currentDraw,
          battery: Math.max(0, current.battery - batteryDrop),
          range: Math.max(0, current.range - 0.015 * modeFactor),
          batteryTemp: current.batteryTemp + (Math.random() - 0.46) * (0.1 + modeFactor * 0.05),
          motorTemp: current.motorTemp + (Math.random() - 0.44) * (0.18 + modeFactor * 0.12),
          controllerTemp: current.controllerTemp + (Math.random() - 0.5) * 0.12,
          ambientTemp: current.ambientTemp + (Math.random() - 0.5) * 0.04,
          rpm: Math.round(speed * (44 + modeFactor * 6) + (Math.random() - 0.5) * 120),
          power: currentDraw * 0.39,
          regen: Math.max(0, (55 - speed) * 0.12),
          tripDistance: current.tripDistance + speed / 3600,
          signalStrength: Math.max(30, Math.min(100, current.signalStrength + (Math.random() - 0.52) * 2.6)),
          latency: Math.max(8, current.latency + (Math.random() - 0.48) * 7.4),
          heading: (current.heading + (Math.random() - 0.5) * 7 + 360) % 360,
        }
        setTrack((points) => {
          const head = points[points.length - 1]
          const rad = next.heading * (Math.PI / 180)
          // km travelled this 1s tick -> degrees of lat/lon
          const km = next.speed / 3600
          const dLat = (km / 111.32) * Math.cos(rad)
          const dLon = (km / (111.32 * Math.cos(head.lat * Math.PI / 180))) * Math.sin(rad)
          return [...points.slice(-119), { lat: head.lat + dLat, lon: head.lon + dLon }]
        })
        return next
      })
    }, 1000)
    return () => clearInterval(timer)
  }, [driveMode])

  const time = useMemo(
    () => now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }),
    [now],
  )
  const charging = telemetry.regen > 0.5

  return (
    <div className="app">
      <main className="grid">
        <div className="left-col">
          <SpeedDial speed={telemetry.speed} driveMode={driveMode} setDriveMode={setDriveMode}
            rpm={telemetry.rpm} power={telemetry.power} />

          <BatteryHero
            battery={telemetry.battery}
            range={telemetry.range}
            voltage={telemetry.voltage}
            current={telemetry.current}
            power={telemetry.power}
            charging={charging}
          />

          <section className="card thermal">
            <h2>
              <Icon name="temp" size={13} />THERMAL
              <span className={`link-pill ${telemetry.signalStrength > 65 ? 'ok' : 'warn'}`}>
                <i className="pulse-dot" />
                {telemetry.signalStrength.toFixed(0)}%
              </span>
              <span className="clock">{time}</span>
            </h2>
            <div className="temp-list">
              <TempBar label="Battery" value={telemetry.batteryTemp} max={70} warn={45} />
              <TempBar label="Motor" value={telemetry.motorTemp} max={100} warn={70} />
              <TempBar label="Controller" value={telemetry.controllerTemp} max={90} warn={65} />
              <TempBar label="Ambient" value={telemetry.ambientTemp} max={50} warn={40} />
            </div>
          </section>

        </div>

        <CameraView speed={telemetry.speed} heading={telemetry.heading} time={time} />

        <MapView track={track} heading={telemetry.heading}
          speed={telemetry.speed} distance={telemetry.tripDistance} />

      </main>
    </div>
  )
}

export default App
