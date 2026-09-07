import Icon from './Icon'
import { getStatusClass, formatRange, MAX_SPEED } from '../data/fleet'
import './TelemetryPanel.css'

const ECO_LIMIT = 60
const TEMP_MIN = 15
const TEMP_MAX = 45

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
  const activeMode = speed <= ECO_LIMIT ? 'ECO' : 'POWER'

  return <p className={`drive-mode ${activeMode.toLowerCase()}`}>{activeMode}</p>
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

export default function TelemetryPanel({ car, alertCount }) {
  const tempRatio = clamp((car.temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN), 0, 1) * 100

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
        <div className="range-split">
          <div><span>ECO</span><b>{formatRange(car.ecoRangeKm)}</b></div>
          <div><span>POWER</span><b>{formatRange(car.powerRangeKm)}</b></div>
        </div>
      </article>

      <article className="metric-card temp-card">
        <div className="metric-title"><span>BATTERY TEMP.</span><Icon name="thermo" size={17} /></div>
        <div className="temperature">
          <strong>{car.temp}<sup>°C</sup></strong>
          <div className="temp-scale"><i style={{ left: `${tempRatio}%` }} /></div>
          <div className="temp-labels"><span>15°</span><span>25°</span><span>35°</span><span>45°</span></div>
        </div>
      </article>
    </div>

    <DriveModes speed={car.speed} />

    <div className="vehicle-summary">
      <div className="odometer"><span>ODOMETER</span><b>{car.odometer}</b></div>
      <div><span>ACTIVE ALERTS</span><b className={alertCount > 0 ? 'alert-count' : ''}>{String(alertCount).padStart(2, '0')}</b></div>
    </div>
  </section>
}
