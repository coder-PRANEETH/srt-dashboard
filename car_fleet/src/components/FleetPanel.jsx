import Icon from './Icon'
import { getStatusClass } from '../data/fleet'
import './FleetPanel.css'

function VehicleCard({ vehicle, selected, onSelect }) {
  return <button
    type="button"
    className={`vehicle ${selected ? 'selected' : ''}`}
    aria-pressed={selected}
    onClick={() => onSelect(vehicle.id)}
  >
    <div className="car-visual"><span className="car-body" /><span className="car-trail" /></div>

    <div className="vehicle-info">
      <div className="vehicle-name">
        <b>{vehicle.name}</b>
        <span>{vehicle.id} · {vehicle.type}</span>
      </div>
      <div className="vehicle-meta">
        <span className={`status ${getStatusClass(vehicle.status)}`}><i />{vehicle.status}</span>
        <strong className={vehicle.charge <= 30 ? 'low' : ''}><Icon name="bolt" size={16} />{vehicle.charge}%</strong>
      </div>
      <div className="charge-track"><i style={{ width: `${vehicle.charge}%` }} /></div>
    </div>
  </button>
}

export default function FleetPanel({ fleet, activeId, onSelect, onClose }) {
  return <div className="fleet-panel panel">
    <div className="panel-heading">
      <div>
        <p className="eyebrow">ACTIVE ASSETS</p>
        <h2>My Fleet <span>{fleet.length} vehicles</span></h2>
      </div>
      <button className="mini-button" type="button" onClick={onClose} aria-label="Close fleet selector">
        <Icon name="close" size={18} />
      </button>
    </div>

    <div className="vehicle-list">
      {fleet.map((vehicle) => <VehicleCard
        key={vehicle.id}
        vehicle={vehicle}
        selected={vehicle.id === activeId}
        onSelect={onSelect}
      />)}
    </div>

    <button className="view-all" type="button">VIEW ALL VEHICLES <Icon name="arrow" size={17} /></button>
  </div>
}
