import Icon from './Icon'
import { fleet } from '../data/fleet'
import './MapPanel.css'

const positions = fleet.map(({ position }) => position)
const latMin = Math.min(...positions.map(([lat]) => lat)) - .008
const latMax = Math.max(...positions.map(([lat]) => lat)) + .008
const lonMin = Math.min(...positions.map(([, lon]) => lon)) - .008
const lonMax = Math.max(...positions.map(([, lon]) => lon)) + .008

function project([lat, lon]) {
  return {
    x: 30 + (lon - lonMin) / (lonMax - lonMin) * 340,
    y: 290 - (lat - latMin) / (latMax - latMin) * 260,
  }
}

export default function MapPanel({ car, showMap, onToggleMap }) {
  const [latitude, longitude] = car.position

  if (!showMap) {
    return <section className="map-panel panel collapsed">
      <button className="view-location" type="button" aria-expanded={false} onClick={onToggleMap}>
        <Icon name="pin" size={18} />VIEW LOCATION
      </button>
    </section>
  }

  return <section className="map-panel panel">
    <div className="map-header">
      <div>
        <p className="eyebrow">OFFLINE LOCATION</p>
        <h2>New Delhi, India</h2>
      </div>
      <span className="live-pill">SAVED POSITIONS</span>
    </div>

    <div className="map-wrap">
      <svg className="fleet-location-plot" viewBox="0 0 400 320" role="img" aria-label={`Relative fleet positions; ${car.name} selected at ${latitude.toFixed(3)}, ${longitude.toFixed(3)}`}>
        <defs>
          <pattern id="location-grid" width="40" height="40" patternUnits="userSpaceOnUse">
            <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#dbe2e9" strokeWidth="1" />
          </pattern>
        </defs>
        <rect width="400" height="320" fill="#f7f9fb" />
        <rect width="400" height="320" fill="url(#location-grid)" />
        <text x="16" y="23" className="plot-direction">N ↑</text>
        {fleet.map((vehicle) => {
          const { x, y } = project(vehicle.position)
          const selected = vehicle.id === car.id
          return <g key={vehicle.id} className={selected ? 'plot-vehicle selected' : 'plot-vehicle'}>
            <circle cx={x} cy={y} r={selected ? 12 : 7} />
            <text x={x + 14} y={y - 9}>{vehicle.id}</text>
          </g>
        })}
      </svg>
      <div className="map-credit">Relative GPS positions · offline</div>
    </div>

    <div className="map-footer">
      <div><Icon name="pin" size={19} /><span>Near <b>{car.place}</b> · {latitude.toFixed(4)}, {longitude.toFixed(4)}</span></div>
      <button className="view-location" type="button" aria-expanded onClick={onToggleMap}>
        HIDE LOCATION <Icon name="arrow" size={16} />
      </button>
    </div>
  </section>
}
