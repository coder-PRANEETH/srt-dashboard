import Icon from './Icon'
import { getStatusClass, formatRange, MAX_SPEED } from '../data/fleet'
import './TelemetryPanel.css'

const ECO_LIMIT = 60
const TEMP_MIN = 15
const TEMP_MAX = 45

const driveModes = [
  { name: 'ECO MODE', range: `0–${ECO_LIMIT} KM/H`, key: 'eco' },
  { name: 'POWER MODE', range: `${ECO_LIMIT + 1}–${MAX_SPEED} KM/H`, key: 'power' },
]

/* Gauge arc: r=45 → circumference ≈ 283. We draw a 270° arc (0.75 of it). */
const RADIUS = 45
const CIRCUMFERENCE = 2 * Math.PI * RADIUS
const ARC = CIRCUMFERENCE * 0.75

const clamp = (value, min, max) => Math.min(Math.max(value, min), max)

function SpeedGauge({ speed }) {
  const ratio = clamp(speed, 0, MAX_SPEED) / MAX_SPEED
  const offset = ARC - ratio * ARC

  return <div className="gauge-wrap">
    <svg className="gauge" viewBox="0 0 120 120" role="img" aria-label={`Speed ${speed} of ${MAX_SPEED} km/h`}>
      <defs>
        <linearGradient id="speedGradient" x1="0" x2="1">
          <stop stopColor="#6b0d12" />
          <stop offset=".55" stopColor="#ff2d33" />
          <stop offset="1" stopColor="#ff5b57" />
        </linearGradient>
      </defs>
      <circle className="gauge-base" cx="60" cy="60" r={RADIUS} strokeDasharray={`${ARC} ${CIRCUMFERENCE}`} />
      <circle
        className="gauge-progress"
        cx="60" cy="60" r={RADIUS}
        strokeDasharray={`${ARC} ${CIRCUMFERENCE}`}
        style={{ strokeDashoffset: offset }}
      />
    </svg>
    <div className="gauge-value"><strong>{speed}</strong><span>KM/H</span></div>
  </div>
}

function DriveModes({ speed }) {
  const activeMode = speed <= ECO_LIMIT ? 'eco' : 'power'

  return <div className="drive-modes" aria-label={`Drive mode: ${activeMode}`}>
    {driveModes.map((mode) => <div
      className={`drive-mode ${mode.key} ${mode.key === activeMode ? 'active' : ''}`}
      key={mode.key}
    >
      <span className="drive-mode-name">{mode.name}</span>
      <b className="drive-mode-range">{mode.range}</b>
      <em className="drive-mode-flag">{mode.key === activeMode ? 'ENGAGED' : 'STANDBY'}</em>
    </div>)}
  </div>
}

function BatteryTank({ value }) {
  return <div className="tank-shell">
    <div className="tank-cap" />
    <div className="tank" role="img" aria-label={`Battery ${value} percent`}>
      <div className="tank-lines" />
      <div className={`charge-liquid ${value <= 30 ? 'low' : ''}`} style={{ height: `${value}%` }}><span /></div>
    </div>
  </div>
}

function MetricFooter({ label, value, accent = false }) {
  return <div className="metric-bottom"><span>{label}</span><b className={accent ? 'accent-text' : ''}>{value}</b></div>
}

export default function TelemetryPanel({ car, alertCount }) {
  const tempRatio = clamp((car.temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN), 0, 1) * 100
  const tempHot = car.temp >= 38

  return <section className="telemetry-column" aria-label="Vehicle telemetry">
    <div className="selected-vehicle panel">
      <div>
        <p className="eyebrow">SELECTED VEHICLE</p>
        <h2>{car.name} <span className="vehicle-id">{car.id}</span></h2>
      </div>
      <span className={`status ${getStatusClass(car.status)}`}><i />{car.status}</span>
    </div>

    <div className="telemetry-grid">
      <article className="metric-card speed-card">
        <div className="metric-title"><span>SPEED</span><Icon name="dots" size={18} /></div>
        <SpeedGauge speed={car.speed} />
        <MetricFooter label="CRUISE" value={car.speed > 0 ? 'ENGAGED' : 'OFF'} accent={car.speed > 0} />
      </article>

      <article className="metric-card battery-card">
        <div className="metric-title"><span>SOC / BATTERY</span><Icon name="bolt" size={17} /></div>
        <div className="battery-main">
          <BatteryTank value={car.charge} />
          <div className="battery-read">
            <strong>{car.charge}<sup>%</sup></strong>
            <p>{formatRange(car.rangeKm)} <span>available</span></p>
          </div>
        </div>
        <MetricFooter label="DRIVING RANGE" value={formatRange(car.rangeKm)} accent />
      </article>

      <article className="metric-card temp-card">
        <div className="metric-title"><span>BATTERY TEMP.</span><Icon name="thermo" size={17} /></div>
        <div className="temperature">
          <strong>{car.temp}<sup>°C</sup></strong>
          <div className="temp-scale"><i style={{ left: `${tempRatio}%` }} /></div>
          <div className="temp-labels"><span>15°</span><span>25°</span><span>35°</span><span>45°</span></div>
        </div>
        <MetricFooter label="SYSTEM" value={tempHot ? 'THERMAL LOAD' : 'NOMINAL'} accent={tempHot} />
      </article>
    </div>

    <DriveModes speed={car.speed} />

    <div className="vehicle-summary">
      <div><span>ODOMETER</span><b>{car.odometer}</b></div>
      <div><span>DRIVER</span><b>{car.driver}</b></div>
      <div><span>ACTIVE ALERTS</span><b className={alertCount > 0 ? 'alert-count' : ''}>{String(alertCount).padStart(2, '0')}</b></div>
    </div>
  </section>
}
