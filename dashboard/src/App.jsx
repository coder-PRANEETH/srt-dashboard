import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import L from 'leaflet'
import 'leaflet/dist/leaflet.css'
import './App.css'

const MAX_SPEED = 160

/*
 * SASTRA Deemed University, Thirumalaisamudram.
 *
 * Verified against OpenStreetMap. The
 * map starts here and stays here until
 * the GPS delivers a real fix.
 */
const START_POS = {
  lat: 10.7280,
  lon: 79.0195,
}

/*
 * Offline tiles live in public/tiles and
 * are served by this same ESP32. No
 * internet exists in the vehicle, so the
 * zoom range below must match whatever
 * tools/download-tiles.py actually
 * fetched.
 */
const TILE_URL = '/tiles/{z}/{x}/{y}.png'
const MIN_ZOOM = 13
const MAX_ZOOM = 17
const DEFAULT_ZOOM = 16

/*
 * Downloaded radius around the campus.
 * Panning past this shows empty tiles,
 * so the map is clamped to it.
 */
const TILE_RADIUS_KM = 2

/* Drop fixes that jump impossibly far. */
const MAX_JUMP_KM = 2
/* Physical sanity check for a single point, in addition to the sender. */
const MAX_TRACK_SPEED_MPS = 55

/* Keep the drawn trail bounded. */
const MAX_TRACK_POINTS = 600

/*
 * Don't draw a trail point for anything under 3 m.
 *
 * This used to be 0.2 m, which is far below what a consumer GPS can
 * actually resolve - so ordinary receiver scatter (a few metres even
 * when perfectly still) was drawn as real movement and the trail
 * fuzzed while parked. The sender now filters too; this is the second
 * line of defence.
 */
const MIN_PLOT_DISTANCE_KM = 0.003

/*
 * Leave the ESP32 time to serve map tiles and drain its CAN controller.
 * The dial is sampled at 10 Hz and GPS at 2 Hz (the sender publishes at
 * 1 Hz). Visual changes still commit on the next animation frame.
 */
const DIAL_POLL_INTERVAL_MS = 100
const GPS_POLL_INTERVAL_MS = 500
const API_TIMEOUT_MS = 3000

async function readApiJson(path, signal) {
  const request = new AbortController()
  const abort = () => request.abort()
  signal.addEventListener('abort', abort, { once: true })
  if (signal.aborted) abort()
  const timeout = setTimeout(abort, API_TIMEOUT_MS)

  try {
    const response = await fetch(path, {
      cache: 'no-store',
      signal: request.signal,
    })
    if (!response.ok) throw new Error(`${path} returned ${response.status}`)
    return await response.json()
  } finally {
    clearTimeout(timeout)
    signal.removeEventListener('abort', abort)
  }
}

function waitForPoll(ms, signal) {
  return new Promise((resolve) => {
    if (signal.aborted || ms <= 0) {
      resolve()
      return
    }
    const finish = () => {
      clearTimeout(timer)
      signal.removeEventListener('abort', finish)
      resolve()
    }
    const timer = setTimeout(finish, ms)
    signal.addEventListener('abort', finish, { once: true })
  })
}

const initialTelemetry = {
  speed: null,
}

/* ---------- geo helpers ---------- */

/* Great-circle distance in km. */
function distanceKm(a, b) {

  const R = 6371

  const dLat =
    ((b.lat - a.lat) * Math.PI) / 180

  const dLon =
    ((b.lon - a.lon) * Math.PI) / 180

  const lat1 =
    (a.lat * Math.PI) / 180

  const lat2 =
    (b.lat * Math.PI) / 180

  const h =
    Math.sin(dLat / 2) ** 2 +
    Math.sin(dLon / 2) ** 2 *
      Math.cos(lat1) *
      Math.cos(lat2)

  return (
    2 *
    R *
    Math.asin(Math.sqrt(h))
  )
}

/* Initial bearing a -> b, degrees. */
function bearingDeg(a, b) {

  const lat1 =
    (a.lat * Math.PI) / 180

  const lat2 =
    (b.lat * Math.PI) / 180

  const dLon =
    ((b.lon - a.lon) * Math.PI) / 180

  const y =
    Math.sin(dLon) * Math.cos(lat2)

  const x =
    Math.cos(lat1) * Math.sin(lat2) -
    Math.sin(lat1) *
      Math.cos(lat2) *
      Math.cos(dLon)

  return (
    (Math.atan2(y, x) * 180) /
      Math.PI +
    360
  ) % 360
}

/*
 * A GPS reading is only worth plotting
 * once it is a real 2D/3D fix. Ready-to-Sky
 * modules emit 0,0 and null-island values
 * while they are still acquiring.
 */
function isValidFix(fix) {

  if (!fix) {
    return false
  }

  const { lat, lon } = fix

  if (
    !Number.isFinite(lat) ||
    !Number.isFinite(lon)
  ) {
    return false
  }

  if (
    Math.abs(lat) > 90 ||
    Math.abs(lon) > 180
  ) {
    return false
  }

  /* Null island = no fix yet. */
  if (
    Math.abs(lat) < 0.0001 &&
    Math.abs(lon) < 0.0001
  ) {
    return false
  }

  return true
}

/* ---------- animation helpers ---------- */

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

/* ---------- icons ---------- */

function Icon({ name, size = 16 }) {
  const paths = {
    bolt: (
      <path d="m13 2-9 12h7l-1 8 9-12h-7l1-8Z" />
    ),

    battery: (
      <>
        <rect x="2" y="7" width="17" height="10" rx="2.5" />
        <path d="M22 11v2" />
      </>
    ),

    wifi: (
      <>
        <path d="M5 12.5a11 11 0 0 1 14 0M8 16a6.5 6.5 0 0 1 8 0" />
        <circle cx="12" cy="19.5" r="1" />
      </>
    ),

    gauge: (
      <>
        <path d="M4.9 17a8 8 0 1 1 14.2 0" />
        <path d="m12 13 3-3" />
      </>
    ),

    temp: (
      <>
        <path d="M14 14.8V4a2 2 0 1 0-4 0v10.8a4 4 0 1 0 4 0Z" />
      </>
    ),

    road: (
      <>
        <path d="M4 20 8 4M20 20 16 4M12 5v3M12 11v3M12 17v3" />
      </>
    ),

    alert: (
      <>
        <path d="M12 3 2 20h20L12 3Z" />
        <path d="M12 10v4M12 17.5v.5" />
      </>
    ),

    camera: (
      <>
        <path d="M3 8.5A2.5 2.5 0 0 1 5.5 6h1.8l1.2-2h6.6l1.2 2h1.8A2.5 2.5 0 0 1 20.5 8.5v8A2.5 2.5 0 0 1 18 19H5.5A2.5 2.5 0 0 1 3 16.5Z" />
        <circle cx="11.75" cy="12" r="3.4" />
      </>
    ),

    map: (
      <>
        <path d="M9 4 3 6.5v13L9 17l6 2.5 6-2.5v-13L15 6.5Z" />
        <path d="M9 4v13M15 6.5v13" />
      </>
    ),
  }

  return (
    <svg
      className="icon"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {paths[name]}
    </svg>
  )
}

/* ---------- cluster gauge ---------- */

/*
 * Both large dials in the binnacle are the same instrument: a 240 deg
 * sweep opening at the bottom, a tick ring inside it and a numeric core.
 * Only the range, label and corner captions differ.
 */
function ClusterGauge({
  value,
  max,
  unit,
  label,
  decimals = 0,
  speed = 0.16,
}) {
  const shown = useAnimatedNumber(value ?? 0, speed)

  const pct = Math.min(
    1,
    Math.max(0, shown / max)
  )

  const R = 128

  const SWEEP = 240

  const ARC =
    (SWEEP / 360) *
    2 *
    Math.PI *
    R

  /*
   * 41 ticks reads as a machined ring at
   * cluster size; every fifth is long.
   */
  const TICKS = 41

  const ticks = Array.from(
    { length: TICKS },
    (_, i) => i
  )

  return (
    <div className="gauge">

      <svg
        viewBox="0 0 300 300"
        className="gauge-svg"
      >

        <circle
          className="gauge-bowl"
          cx="150"
          cy="150"
          r="140"
        />

        <circle
          className="gauge-track"
          cx="150"
          cy="150"
          r={R}
          strokeDasharray={`${ARC} 999`}
          transform="rotate(150 150 150)"
        />

        <circle
          className="gauge-fill"
          cx="150"
          cy="150"
          r={R}
          strokeDasharray={`${ARC * pct} 999`}
          transform="rotate(150 150 150)"
        />

        {ticks.map((i) => {

          const frac = i / (TICKS - 1)

          const a =
            (150 + frac * SWEEP) *
            (Math.PI / 180)

          const major = i % 5 === 0

          const inner = major ? 92 : 100

          return (
            <line
              key={i}
              className={`gauge-tick ${
                major ? 'major' : ''
              } ${
                frac <= pct ? 'on' : ''
              }`}
              x1={150 + Math.cos(a) * inner}
              y1={150 + Math.sin(a) * inner}
              x2={150 + Math.cos(a) * 110}
              y2={150 + Math.sin(a) * 110}
            />
          )
        })}

      </svg>

      <div className="gauge-core">

        <strong className="gauge-value">
          {value === null ? '—' : shown.toFixed(decimals)}
        </strong>

        <span className="gauge-unit">
          {unit}
        </span>

        <span className="gauge-label">
          {label}
        </span>

      </div>

   

    </div>
  )
}

/* ---------- thermal ---------- */



/* ---------- camera ---------- */

/*
 * Centre of the binnacle: the forward
 * view with the car sitting on the lane,
 * as on a lane-keeping cluster.
 */
function CameraView({
  gps,
  detail,
  heading,
  time,
  distance,
  onOpenMap,
}) {
  return (
    <div className="camera">
      <div className="camera-heading">
        <span className="camera-label">GPS / ROUTE</span>
        <span className={`gps-pill ${gps.state}`}><i className="pulse-dot" />{gps.label}</span>
      </div>
      <div className="camera-position">
        {detail.position
          ? `${detail.position.lat.toFixed(6)}, ${detail.position.lon.toFixed(6)}`
          : 'Awaiting position'}
      </div>
      <div className="camera-stats">
        <div><span>Satellites</span><strong>{detail.satellites ?? '—'}</strong></div>
        <div><span>HDOP</span><strong>{detail.hdop?.toFixed(1) ?? '—'}</strong></div>
        <div><span>Heading</span><strong>{detail.position ? `${Math.round(heading)}°` : '—'}</strong></div>
        <div><span>Trip</span><strong>{distance.toFixed(2)} km</strong></div>
      </div>
      <div className="camera-footer"><span>{time}</span><span>LOCAL GPS · OFFLINE MAP</span></div>
      <button
        type="button"
        className="mapview"
        onClick={onOpenMap}
      >
        VIEW ROUTE
      </button>
    </div>
  )
}
/* ---------- map ---------- */

function MapView({
  track,
  heading,
  speed,
  distance,
  gps,
  expanded,
  onToggle,
}) {
  const holder = useRef(null)
  const map = useRef(null)
  const trail = useRef(null)
  const marker = useRef(null)
  const renderedTrackLength = useRef(0)

  const last =
    track[track.length - 1] ??
    START_POS

  useEffect(() => {

    if (
      map.current ||
      !holder.current
    ) {
      return
    }

    /*
     * Clamp panning to the region we
     * actually downloaded tiles for.
     */
    const dLat =
      TILE_RADIUS_KM / 111.32

    const dLon =
      TILE_RADIUS_KM /
      (111.32 *
        Math.cos(
          (START_POS.lat * Math.PI) /
            180
        ))

    const bounds = L.latLngBounds(
      [
        START_POS.lat - dLat,
        START_POS.lon - dLon,
      ],
      [
        START_POS.lat + dLat,
        START_POS.lon + dLon,
      ]
    )

    map.current = L.map(
      holder.current,
      {
        center: [
          START_POS.lat,
          START_POS.lon,
        ],
        zoom: DEFAULT_ZOOM,
        minZoom: MIN_ZOOM,
        maxZoom: MAX_ZOOM,
        maxBounds: bounds,
        maxBoundsViscosity: 1,
        zoomControl: false,
        attributionControl: true,
        dragging: false,
        scrollWheelZoom: false,
        doubleClickZoom: false,
        touchZoom: false,
        keyboard: false,
      }
    )

    /*
     * Tiles are served from this ESP32's
     * own filesystem, never the internet.
     */
    L.tileLayer(TILE_URL, {
      minZoom: MIN_ZOOM,
      maxZoom: MAX_ZOOM,
      /*
       * Stadia's terms require crediting
       * them and OpenMapTiles alongside
       * OSM wherever their tiles appear.
       */
      attribution:
        '&copy; Stadia Maps &copy; OpenMapTiles &copy; OpenStreetMap',
      /*
       * Any gap in the offline set would
       * otherwise render as a broken
       * image icon.
       */
      errorTileUrl:
        'data:image/svg+xml;charset=utf-8,' +
        encodeURIComponent(
          '<svg xmlns="http://www.w3.org/2000/svg" ' +
            'width="256" height="256">' +
            '<rect width="256" height="256" fill="#eef1f5"/>' +
            '</svg>'
        ),
    }).addTo(map.current)

    trail.current =
      L.polyline([], {
        color: '#0d6fd8',
        weight: 4,
        opacity: 0.9,
        lineJoin: 'round',
      }).addTo(map.current)

    marker.current =
      L.marker(
        [
          START_POS.lat,
          START_POS.lon,
        ],
        {
          icon: L.divIcon({
            className:
              'map-marker',
            html:
              '<div class="map-marker-ping"></div><div class="map-marker-arrow"></div>',
            iconSize: [26, 26],
            iconAnchor: [13, 13],
          }),
          interactive: false,
        }
      ).addTo(map.current)

    const ro =
      new ResizeObserver(
        () =>
          map.current?.invalidateSize()
      )

    ro.observe(holder.current)

    return () => {

      ro.disconnect()

      map.current?.remove()

      map.current = null
      renderedTrackLength.current = 0

    }

  }, [])

  useEffect(() => {

    if (!map.current) {
      return
    }

    /* Adding only new points avoids rebuilding a 600-point Leaflet path. */
    if (track.length <= renderedTrackLength.current) {
      trail.current?.setLatLngs(
        track.map((p) => [p.lat, p.lon])
      )
    } else {
      for (let i = renderedTrackLength.current; i < track.length; i += 1) {
        trail.current?.addLatLng([track[i].lat, track[i].lon])
      }
    }
    renderedTrackLength.current = track.length

    marker.current?.setLatLng([
      last.lat,
      last.lon,
    ])

    /* Follow each fix without queuing pan animations behind it. */
    map.current.panTo(
      [
        last.lat,
        last.lon,
      ],
      {
        animate: false,
        noMoveStart: true,
      }
    )

    const arrow =
      marker.current
        ?.getElement()
        ?.querySelector(
          '.map-marker-arrow'
        )

    if (arrow) {
      arrow.style.transform =
        `rotate(${heading}deg)`
    }

  }, [
    track,
    last.lat,
    last.lon,
    heading,
  ])

  return (
    <section
      className={`card mapcard ${
        expanded ? 'expanded' : ''
      }`}
    >

      <h2>

        <Icon
          name="map"
          size={13}
        />

        ROUTE

        <span
          className={`gps-pill ${gps.state}`}
        >

          <i className="pulse-dot" />

          {gps.label}

        </span>

        <span className="map-dist">
          {distance.toFixed(1)} km
        </span>

      </h2>

      {/*
        * The map face IS the button. Making the element itself a
        * <button> avoids layering anything over Leaflet, so there is
        * no z-index race with its panes and controls.
        */}
      <button
        type="button"
        className="map-face"
        onClick={onToggle}
        aria-pressed={expanded}
        aria-label={
          expanded
            ? 'Exit fullscreen map'
            : 'Expand map to fullscreen'
        }
      >

        <div
          ref={holder}
          className="map-canvas"
        />

        <span className="map-zoom-hint">
          {expanded ? 'CLOSE' : 'EXPAND'}
        </span>

        <div className="map-speed">

          {Math.round(speed)}

          <i>km/h</i>

        </div>

        {gps.state !== 'ok' && (

          <div className="map-nofix">

            {gps.state === 'stale'
              ? 'GPS SIGNAL LOST'
              : 'ACQUIRING GPS FIX'}

          </div>

        )}

      </button>

    </section>
  )
}

/* ---------- app ---------- */

function canU16(data, offset) {
  return data[offset] | (data[offset + 1] << 8)
}

function canI32(data, offset) {
  return data[offset] | (data[offset + 1] << 8) |
    (data[offset + 2] << 16) | (data[offset + 3] << 24)
}

function describeCanFrame(frame) {
  const data = frame.data
  if (!Array.isArray(data) || data.length !== 8) return '—'

  if (frame.id === 0x100) {
    return `Position: ${(canI32(data, 0) / 1e7).toFixed(6)}, ${(canI32(data, 4) / 1e7).toFixed(6)}`
  }
  if (frame.id === 0x101) {
    const course = canU16(data, 2)
    const hdop = canU16(data, 6)
    return `Speed ${(canU16(data, 0) / 10).toFixed(1)} km/h · Course ${course === 0xFFFF ? 'unknown' : `${(course / 10).toFixed(1)}°`} · ${data[4]} sat · Fix ${data[5] ? 'yes' : 'no'} · HDOP ${hdop ? (hdop / 100).toFixed(2) : 'unknown'}`
  }
  if (frame.id === 0x102) {
    return `Heartbeat ${(canI32(data, 0) >>> 0)} · Sender uptime ${(canI32(data, 4) >>> 0)} s`
  }
  return '—'
}

function formatCanTime(ms) {
  if (!Number.isFinite(ms)) return '—'
  const minutes = Math.floor(ms / 60000)
  const seconds = ((ms % 60000) / 1000).toFixed(1).padStart(4, '0')
  return `${minutes}:${seconds}`
}

const CanLog = memo(function CanLog({ onClose }) {
  const [snapshot, setSnapshot] = useState(null)
  const [error, setError] = useState('')

  useEffect(() => {
    const controller = new AbortController()
    let timer
    let active = true

    async function poll() {
      try {
        const data = await readApiJson('/api/can-log', controller.signal)
        if (!Array.isArray(data.frames)) throw new Error('Invalid CAN log response')
        if (active) {
          setSnapshot(data)
          setError('')
        }
      } catch (err) {
        if (active) setError(err.message)
      } finally {
        if (active) timer = setTimeout(poll, 750)
      }
    }

    poll()
    return () => {
      active = false
      clearTimeout(timer)
      controller.abort()
    }
  }, [])

  useEffect(() => {
    const onKeyDown = (event) => {
      if (event.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [onClose])

  const frames = useMemo(() => [...(snapshot?.frames ?? [])].reverse(), [snapshot])

  return (
    <div className="can-log-overlay" role="presentation">
      <section className="can-log-panel" role="dialog" aria-modal="true" aria-labelledby="can-log-title">
        <header className="can-log-header">
          <div>
            <h2 id="can-log-title">CAN LOG</h2>
            <p>Latest 64 frames received by the dashboard module</p>
          </div>
          <button type="button" className="can-log-close" onClick={onClose} autoFocus aria-label="Close CAN log">Close ×</button>
        </header>
        <div className="can-log-status" aria-live="polite">
          <span className={snapshot?.ready ? 'can-ready' : 'can-waiting'}>
            {snapshot ? (snapshot.ready ? 'CAN MODULE READY' : 'CAN MODULE NOT FOUND') : 'CONNECTING'}
          </span>
          <span>{snapshot ? `${snapshot.total} frames received` : 'Waiting for data'}</span>
          {error && <span className="can-error">{error}</span>}
        </div>
        <div className="can-log-table-wrap">
          <table className="can-log-table">
            <caption>Newest CAN frames first. Time is since the ESP32 started.</caption>
            <thead><tr><th scope="col">Time</th><th scope="col">CAN ID</th><th scope="col">DLC</th><th scope="col">Data (hex)</th><th scope="col">Decoded</th></tr></thead>
            <tbody>
              {frames.map((frame) => (
                <tr key={frame.seq}>
                  <td>{formatCanTime(frame.ms)}</td>
                  <td>0x{Number(frame.id).toString(16).toUpperCase().padStart(3, '0')}</td>
                  <td>{frame.dlc}</td>
                  <td className="can-bytes">{Array.isArray(frame.data) ? frame.data.map((byte) => Number(byte).toString(16).toUpperCase().padStart(2, '0')).join(' ') : '—'}</td>
                  <td>{describeCanFrame(frame)}</td>
                </tr>
              ))}
              {frames.length === 0 && (
                <tr><td colSpan="5" className="can-log-empty">{snapshot ? 'No CAN frames received yet.' : 'Loading CAN frames…'}</td></tr>
              )}
            </tbody>
          </table>
        </div>
      </section>
    </div>
  )
})

function App() {

  const [telemetry, setTelemetry] =
    useState(initialTelemetry)

  const [now, setNow] =
    useState(new Date())

  /*
   * The route map doubles as a button:
   * tapping it lifts the card over the
   * whole screen for a proper look at
   * the trail.
   */
  const [mapExpanded, setMapExpanded] =
    useState(false)
  const [canLogOpen, setCanLogOpen] = useState(false)
  const closeCanLog = useCallback(() => setCanLogOpen(false), [])

  /*
   * Empty until the GPS gives a real
   * fix - we must not draw a fake trail
   * starting at the campus centre.
   */
  const [track, setTrack] =
    useState([])
  const [tripDistance, setTripDistance] = useState(0)

  const [gpsAt, setGpsAt] =
    useState(null)

  const [gpsDetail, setGpsDetail] = useState({
    position: null,
    satellites: null,
    hdop: null,
    speed: null,
    course: null,
  })

  /*
   * Read potentiometer value
   * from ESP32 #1.
   *
   * ESP32 API:
   * /api/data
   *
   * Response:
   * {"value": 75.4}
   */
  useEffect(() => {

    const controller = new AbortController()
    let cancelled = false
    let frame = null
    let pendingValue = null
    let errorLogged = false

    const commit = () => {
      frame = null

      if (cancelled || pendingValue === null) {
        return
      }

      const value = pendingValue
      pendingValue = null

      setTelemetry((current) => ({
        ...current,
        speed: value,
      }))
    }

    const scheduleCommit = () => {
      if (frame === null) {
        frame = requestAnimationFrame(commit)
      }
    }

    const loadValue = async () => {

      try {

        const data = await readApiJson('/api/data', controller.signal)

        if (cancelled) return true

        if (!Number.isFinite(data?.value)) {
          throw new Error('Speed API returned no value')
        }

          pendingValue = Math.max(
            0,
            Math.min(MAX_SPEED, data.value)
          )
          scheduleCommit()

        errorLogged = false
        return true

      } catch (error) {

        if (cancelled) return false
        pendingValue = null
        if (!errorLogged) console.warn('Speed API unavailable:', error)
        errorLogged = true
        if (!cancelled) setTelemetry((current) =>
          current.speed === null ? current : { ...current, speed: null }
        )
        return false

      }

    }

    const poll = async () => {
      while (!cancelled) {
        const startedAt = performance.now()
        const connected = await loadValue()
        const wait = Math.max(
          0,
          (connected ? DIAL_POLL_INTERVAL_MS : 500) - (performance.now() - startedAt)
        )
        await waitForPoll(wait, controller.signal)
      }
    }

    poll()

    return () => {
      cancelled = true
      controller.abort()
      if (frame !== null) {
        cancelAnimationFrame(frame)
      }
    }

  }, [])

  /*
   * Read the GPS fix from ESP32 #2.
   *
   * The Ready-to-Sky module is wired to
   * the second ESP32; that board parses
   * NMEA and this dashboard board
   * re-serves it at:
   *
   * /api/gps
   *
   * Response:
   * {
   *   "lat": 10.7280,
   *   "lon": 79.0195,
   *   "sats": 9,
   *   "hdop": 1.2,
   *   "speed": 34.5,   // km/h, optional
   *   "course": 78.4,  // degrees, optional
   *   "fix": true
   * }
   */
  useEffect(() => {

    const controller = new AbortController()
    let cancelled = false
    let frame = null
    let acceptedTrack = []
    let acceptedDistance = 0
    let trackChanged = false
    let distanceChanged = false
    let pendingGpsAt = null
    let pendingDetail = null
    let lastAcceptedAt = null
    let lastUiRefreshAt = 0
    let errorLogged = false

    const commit = () => {
      frame = null

      if (cancelled) {
        return
      }

      if (trackChanged) {
        trackChanged = false
        setTrack([...acceptedTrack])
      }

      if (distanceChanged) {
        distanceChanged = false
        setTripDistance(acceptedDistance)
      }

      if (pendingGpsAt !== null) {
        setGpsAt(pendingGpsAt)
        pendingGpsAt = null
      }

      if (pendingDetail !== null) {
        setGpsDetail(pendingDetail)
        pendingDetail = null
      }

    }

    const scheduleCommit = () => {
      if (frame === null) {
        frame = requestAnimationFrame(commit)
      }
    }

    const loadFix = async () => {

      try {

        const data = await readApiJson('/api/gps', controller.signal)

        errorLogged = false

        if (cancelled) {
          return true
        }

        /*
         * fix:false means the module is
         * powered but has not locked on
         * yet. Keep the last known trail.
         */
        if (data?.fix === false) {
          pendingGpsAt = null
          setGpsAt(null)
          return true
        }

        if (!isValidFix(data)) {
          return true
        }

        const point = {
          lat: data.lat,
          lon: data.lon,
        }

        const receivedAt = Date.now()
        const fixAt = receivedAt - (
          Number.isFinite(data.ageMs) ? Math.max(0, data.ageMs) : 0
        )

        const previous =
          acceptedTrack[acceptedTrack.length - 1]

        if (!previous) {
          acceptedTrack = [point]
          trackChanged = true
          lastAcceptedAt = receivedAt
        } else {

          const moved = distanceKm(
            previous,
            point
          )

          /*
           * A consumer module under a
           * poor sky occasionally emits a
           * wild outlier. Ignore it
           * rather than drawing a spike
           * across the map.
           */
          const seconds = Math.max(0.1, (receivedAt - lastAcceptedAt) / 1000)
          const speedMps = Number.isFinite(data.speed)
            ? Math.min(MAX_TRACK_SPEED_MPS, Math.max(0, data.speed / 3.6))
            : 0
          const allowedKm = (15 + (speedMps + 8) * seconds) / 1000
          if (moved > MAX_JUMP_KM || moved > allowedKm) {
            scheduleCommit()
            return true
          }

          /*
           * Below ~3 m the movement is
           * receiver noise, not the car.
           * Plotting it makes the trail
           * fuzz while parked.
           */
          if (moved < MIN_PLOT_DISTANCE_KM) {
            /* At speed, repeated HTTP reads are the same 1 Hz GPS sample.
             * Keep its original time so the next real movement gets its
             * full elapsed-time allowance. While parked, refresh the clock
             * so a late isolated jump never becomes plausible. */
            if (!Number.isFinite(data.speed) || data.speed < 3) {
              lastAcceptedAt = receivedAt
            }
            if (receivedAt - lastUiRefreshAt < 250) return true
            lastUiRefreshAt = receivedAt
            pendingGpsAt = fixAt
            pendingDetail = {
              position: previous,
              satellites: Number.isFinite(data.sats) ? data.sats : null,
              hdop: Number.isFinite(data.hdop) ? data.hdop : null,
              speed: Number.isFinite(data.speed) ? data.speed : null,
              course: Number.isFinite(data.course) ? data.course : null,
            }
            scheduleCommit()
            return true
          }

          acceptedTrack = [
            ...acceptedTrack,
            point,
          ]
          acceptedDistance += moved
          distanceChanged = true

          if (acceptedTrack.length >
            MAX_TRACK_POINTS
          ) {
            acceptedTrack = acceptedTrack.slice(
              acceptedTrack.length - MAX_TRACK_POINTS
            )
          }
          trackChanged = true
          lastAcceptedAt = receivedAt
        }

          lastUiRefreshAt = receivedAt
          pendingGpsAt = fixAt
          pendingDetail = {
            position: point,
            satellites: Number.isFinite(data.sats) ? data.sats : null,
            hdop: Number.isFinite(data.hdop) ? data.hdop : null,
            speed: Number.isFinite(data.speed) ? data.speed : null,
            course: Number.isFinite(data.course) ? data.course : null,
          }

        scheduleCommit()
        return true

      } catch (error) {

        if (cancelled) return false
        if (!errorLogged) console.warn('GPS API unavailable:', error)
        errorLogged = true
        return false

      }

    }

    const poll = async () => {
      while (!cancelled) {
        const startedAt = performance.now()
        const connected = await loadFix()
        const wait = Math.max(
          0,
          (connected ? GPS_POLL_INTERVAL_MS : 500) - (performance.now() - startedAt)
        )
        await waitForPoll(wait, controller.signal)
      }
    }

    poll()

    return () => {
      cancelled = true
      controller.abort()
      if (frame !== null) {
        cancelAnimationFrame(frame)
      }
    }

  }, [])

  /*
   * Clock only.
   *
   * No simulated speed.
   */
  useEffect(() => {

    const timer =
      setInterval(() => {

        setNow(new Date())

      }, 1000)

    return () =>
      clearInterval(timer)

  }, [])

  /* Escape leaves the fullscreen map. */


  const time = useMemo(
    () =>
      now.toLocaleTimeString(
        [],
        {
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
        }
      ),
    [now]
  )

  /*
   * Heading shown on the map: the
   * module's course when we have one,
   * otherwise the bearing between the
   * last two plotted points.
   */
  const heading = useMemo(() => {

    if (Number.isFinite(gpsDetail.course)) {
      return ((gpsDetail.course % 360) + 360) % 360
    }

    if (track.length >= 2) {
      return bearingDeg(
        track[track.length - 2],
        track[track.length - 1]
      )
    }

    return 0

  }, [track, gpsDetail.course])

  /*
   * A fix older than 5 s means the link
   * to ESP32 #2 or the sky view dropped.
   */
  const gps = useMemo(() => {

    if (!gpsAt) {
      return {
        state: gpsDetail.position ? 'stale' : 'searching',
        label: gpsDetail.position ? 'STALE' : 'NO FIX',
      }
    }

    if (now.getTime() - gpsAt > 5000) {
      return {
        state: 'stale',
        label: 'STALE',
      }
    }

    return {
      state: 'ok',
      label: Number.isFinite(
        gpsDetail.satellites
      )
        ? `${gpsDetail.satellites} SAT`
        : 'FIX',
    }

  }, [
    gpsAt,
    now,
    gpsDetail.satellites,
    gpsDetail.position,
  ])

  return (
    <div className="app">

      <div className="binnacle">
            
        <div className="cluster">

          {/* ---- top rail ---- */}

          <header className="cluster-top">

            <div className="cluster-title">SRT <span>LOCAL TELEMETRY</span></div>
            <div className="cluster-actions">
              <span className="cluster-network">ESP32 ACCESS POINT</span>
              <button type="button" className="can-log-button" onClick={() => setCanLogOpen(true)}>CAN LOG</button>
            </div>

          </header>

          {/* ---- instruments ---- */}

          <div className="cluster-body">

            <ClusterGauge
              value={gps.state === 'ok' ? gpsDetail.speed : null}
              max={MAX_SPEED}
              unit="km/h"
              label="GPS SPEED"
              speed={0.14}
            />

            <div className="cluster-mid">

              <CameraView
                gps={gps}
                detail={gpsDetail}
                heading={heading}
                time={time}
                distance={tripDistance}
                onOpenMap={() =>
                  setMapExpanded(true)
                }
              />

            </div>

            <ClusterGauge
              value={telemetry.speed}
              max={MAX_SPEED}
              unit="km/h"
              label="DIAL SPEED"
              speed={0.18}
            />

              
           
           
          </div>

        </div>

      

      {/* ---- secondary deck: route + thermal ---- */}

      <div className="deck">
              {
        <MapView
          track={track}
          heading={heading}
          speed={gps.state === 'ok' ? gpsDetail.speed ?? 0 : 0}
          distance={tripDistance}
          gps={gps}
          expanded={mapExpanded}
          onToggle={() =>
            setMapExpanded(
              (open) => !open
            )
          }
        />}

      

      </div>

      {canLogOpen && <CanLog onClose={closeCanLog} />}

    </div>
    </div>
  )
}

export default App
