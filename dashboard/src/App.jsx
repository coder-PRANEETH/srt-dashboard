import { useEffect, useMemo, useRef, useState } from 'react'
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

/* Keep the drawn trail bounded. */
const MAX_TRACK_POINTS = 600

/* Preserve motion visible at a 5 ms source cadence. */
const MIN_PLOT_DISTANCE_KM = 0.0002

/*
 * The ESPs can publish much faster than the browser can paint.  Fetch the
 * newest sample every 5 ms when the link permits it, but commit visual work
 * on the next animation frame (normally 60 Hz).  This keeps the dashboard
 * responsive while retaining every accepted GPS point in the trail.
 */
const POLL_INTERVAL_MS = 5

const initialTelemetry = {
  speed: 0,
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
  signalStrength: 92,
  latency: 18,
  heading: 78,
  satellites: null,
}

const driveModes = ['Eco', 'Normal', 'Sport']

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
  footLeft,
  footRight,
  footMid,
  decimals = 0,
  speed = 0.16,
}) {
  const shown = useAnimatedNumber(value, speed)

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
          {shown.toFixed(decimals)}
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

/* ---------- battery rail ---------- */

/*
 * The right-hand SOC column: a stack of
 * lit segments above the percentage.
 */
function BatteryRail({
  battery,
  charging,
}) {
  const shown = useAnimatedNumber(
    battery,
    0.1
  )

  const level =
    shown > 45
      ? 'good'
      : shown > 18
        ? 'mid'
        : 'low'

  const CELLS = 8

  const pct =
    Math.max(
      0,
      Math.min(100, shown)
    ) / 100

  const litCells = Math.round(
    pct * CELLS
  )

  return (
    <div
      className={`batt-rail level-${level} ${
        charging ? 'charging' : ''
      }`}
    >

      <span className="rail-cap">
        BATTERY
      </span>

      <div className="rail-body">

        <span className="rail-nub" />

        <div className="rail-shell">

          {Array.from(
            { length: CELLS },
            (_, i) => (
              <span
                key={i}
                className={`rail-cell ${
                  i < litCells
                    ? 'lit'
                    : ''
                }`}
              />
            )
          )}

        </div>
        

      </div>

      <strong className="rail-pct">
        {shown.toFixed(0)}%
      </strong>

      <span className="rail-sub">
        SOC
      </span>

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
  speed,
  heading,
  time,
  onOpenMap,
}) {
  const dur = Math.max(
    0.35,
    3.2 -
      (speed / MAX_SPEED) *
        2.7
  )

  return (
    <div className="camera"> 
  <div className="stecon">
      <h2 className='stegreen'>Automatic</h2>
  </div>
      <button
        type="button"
        className="mapview"
        onClick={onOpenMap}
      >
        VIEW MAP
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

    /* A 200 Hz source must never queue pan animations behind itself. */
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

function App() {

  const [telemetry, setTelemetry] =
    useState(initialTelemetry)

  const [now, setNow] =
    useState(new Date())

  const [driveMode, setDriveMode] =
    useState('Normal')

  /*
   * The route map doubles as a button:
   * tapping it lifts the card over the
   * whole screen for a proper look at
   * the trail.
   */
  const [mapExpanded, setMapExpanded] =
    useState(false)

  /*
   * Empty until the GPS gives a real
   * fix - we must not draw a fake trail
   * starting at the campus centre.
   */
  const [track, setTrack] =
    useState([])

  const [gpsAt, setGpsAt] =
    useState(null)

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

    let cancelled = false
    let frame = null
    let pendingValue = null

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

        const response =
          await fetch('/api/data', {
            cache: 'no-store',
          })

        if (!response.ok) {
          throw new Error(
            `API returned ${response.status}`
          )
        }

        const data =
          await response.json()

        if (
          Number.isFinite(
            data?.value
          )
        ) {

          pendingValue = Math.max(
            0,
            Math.min(MAX_SPEED, data.value)
          )
          scheduleCommit()

        }

      } catch (error) {

        console.error(
          'ESP32 connection error:',
          error
        )

      }

    }

    const poll = async () => {
      while (!cancelled) {
        const startedAt = performance.now()
        await loadValue()
        const wait = Math.max(
          0,
          POLL_INTERVAL_MS - (performance.now() - startedAt)
        )
        if (wait > 0) {
          await new Promise((resolve) => setTimeout(resolve, wait))
        }
      }
    }

    poll()

    return () => {
      cancelled = true
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

    let cancelled = false
    let frame = null
    let acceptedTrack = []
    let trackChanged = false
    let pendingGpsAt = null
    let pendingCourse = null
    let pendingSatellites = null

    const commit = () => {
      frame = null

      if (cancelled) {
        return
      }

      if (trackChanged) {
        trackChanged = false
        setTrack([...acceptedTrack])
      }

      if (pendingGpsAt !== null) {
        setGpsAt(pendingGpsAt)
        pendingGpsAt = null
      }

      if (
        pendingCourse !== null ||
        pendingSatellites !== null
      ) {
        const course = pendingCourse
        const satellites = pendingSatellites
        pendingCourse = null
        pendingSatellites = null

        setTelemetry((current) => ({
          ...current,
          ...(course !== null
            ? { heading: course }
            : {}),
          ...(satellites !== null
            ? { satellites }
            : {}),
        }))
      }
    }

    const scheduleCommit = () => {
      if (frame === null) {
        frame = requestAnimationFrame(commit)
      }
    }

    const loadFix = async () => {

      try {

        const response =
          await fetch('/api/gps', {
            cache: 'no-store',
          })

        if (!response.ok) {
          throw new Error(
            `GPS API returned ${response.status}`
          )
        }

        const data =
          await response.json()

        if (cancelled) {
          return
        }

        /*
         * fix:false means the module is
         * powered but has not locked on
         * yet. Keep the last known trail.
         */
        if (
          data?.fix === false ||
          !isValidFix(data)
        ) {
          return
        }

        const point = {
          lat: data.lat,
          lon: data.lon,
        }

        pendingGpsAt = Date.now()

        const previous =
          acceptedTrack[acceptedTrack.length - 1]

        if (!previous) {
          acceptedTrack = [point]
          trackChanged = true
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
          if (moved > MAX_JUMP_KM) {
            scheduleCommit()
            return
          }

          /*
           * Below ~20 cm the movement is
           * receiver noise, not the car.
           * Plotting it makes the trail
           * fuzz while parked.
           */
          if (moved < MIN_PLOT_DISTANCE_KM) {
            scheduleCommit()
            return
          }

          acceptedTrack = [
            ...acceptedTrack,
            point,
          ]

          if (acceptedTrack.length >
            MAX_TRACK_POINTS
          ) {
            acceptedTrack = acceptedTrack.slice(
              acceptedTrack.length - MAX_TRACK_POINTS
            )
          }
          trackChanged = true
        }

          /*
           * Prefer the course the module
           * reports; fall back to the
           * bearing between our last two
           * points.
           */
          if (
            Number.isFinite(
              data.course
            )
          ) {
            pendingCourse = data.course
          }

          if (
            Number.isFinite(
              data.sats
            )
          ) {
            pendingSatellites = data.sats
          }

        scheduleCommit()

      } catch (error) {

        console.error(
          'GPS connection error:',
          error
        )

      }

    }

    const poll = async () => {
      while (!cancelled) {
        const startedAt = performance.now()
        await loadFix()
        const wait = Math.max(
          0,
          POLL_INTERVAL_MS - (performance.now() - startedAt)
        )
        if (wait > 0) {
          await new Promise((resolve) => setTimeout(resolve, wait))
        }
      }
    }

    poll()

    return () => {
      cancelled = true
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

  const charging =
    telemetry.regen > 0.5

  /*
   * Heading shown on the map: the
   * module's course when we have one,
   * otherwise the bearing between the
   * last two plotted points.
   */
  const heading = useMemo(() => {

    if (track.length >= 2) {
      return bearingDeg(
        track[track.length - 2],
        track[track.length - 1]
      )
    }

    return telemetry.heading

  }, [track, telemetry.heading])

  /*
   * Distance actually travelled, summed
   * from the GPS trail rather than the
   * seeded placeholder.
   */
  const tripDistance = useMemo(() => {

    let total = 0

    for (let i = 1; i < track.length; i += 1) {
      total += distanceKm(
        track[i - 1],
        track[i]
      )
    }

    return total

  }, [track])

  /*
   * A fix older than 5 s means the link
   * to ESP32 #2 or the sky view dropped.
   */
  const gps = useMemo(() => {

    if (!gpsAt) {
      return {
        state: 'searching',
        label: 'NO FIX',
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
        telemetry.satellites
      )
        ? `${telemetry.satellites} SAT`
        : 'FIX',
    }

  }, [
    gpsAt,
    now,
    telemetry.satellites,
  ])

  return (
    <div className="app">

      <div className="binnacle">
            
        <div className="cluster">

          {/* ---- top rail: drive modes ---- */}

          <header className="cluster-top">

            <div className="mode-switch">

              {driveModes.map((mode) => (
                <button
                key={mode}
                className={`mode-btn ${
                  mode === driveMode
                  ? 'active'
                  : ''
                }`}
                onClick={() =>
                  setDriveMode(mode)
                }
                >
                  {mode.toUpperCase()}
                </button>

              ))}

            </div>

          </header>

          {/* ---- instruments ---- */}

          <div className="cluster-body">

            <ClusterGauge
              value={telemetry.power}
              max={120}
              unit="kW"
              label="POWER OUTPUT"
              footLeft="CHARGE"
              footMid="0"
              footRight="POWER"
              speed={0.14}
            />

            <div className="cluster-mid">

              <CameraView
                speed={telemetry.speed}
                heading={heading}
                time={time}
                onOpenMap={() =>
                  setMapExpanded(true)
                }
              />

            </div>

            <ClusterGauge
              value={telemetry.speed}
              max={MAX_SPEED}
              unit="km/h"
              label="SPEED"
              footLeft=""
              footMid=""
              footRight=""
              speed={0.18}
            />

            <BatteryRail
              battery={telemetry.battery}
              charging={charging}
            />
              
           
           
          </div>

        </div>

      

      {/* ---- secondary deck: route + thermal ---- */}

      <div className="deck">
              {mapExpanded ?
        <MapView
          track={track}
          heading={heading}
          speed={telemetry.speed}
          distance={tripDistance}
          gps={gps}
          expanded={mapExpanded}
          onToggle={() =>
            setMapExpanded(
              (open) => !open
            )
          }
        />:null}

      

      </div>

    </div>
    </div>
  )
}

export default App
