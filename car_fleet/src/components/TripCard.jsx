import { useEffect, useState } from 'react'
import Icon from './Icon'
import driverFrame from '../assets/driver-current-frame.svg'
import './TripCard.css'

const mobilityOptions = [
  { key: 'MOBILE', label: 'MOBILE', className: 'mobile' },
  { key: 'IMMOBILE', label: 'IMMOBILISE', className: 'immobile' },
  { key: 'FORCE IMMOBILISE', label: 'FORCE IMMOBILISE', className: 'force' },
]

export default function TripCard({ car }) {
  const inTransit = car.status === 'IN TRANSIT'
  const [ignitionOn, setIgnitionOn] = useState(inTransit)
  const [mobility, setMobility] = useState(inTransit ? 'MOBILE' : 'IMMOBILE')
  const [showDriverFrame, setShowDriverFrame] = useState(false)

  useEffect(() => {
    if (!showDriverFrame) return
    const onKey = (event) => { if (event.key === 'Escape') setShowDriverFrame(false) }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [showDriverFrame])

  const state = ignitionOn ? `${mobility} · IGNITION ON` : 'IGNITION OFF'

  return <section className="trip-card panel" aria-labelledby="vehicle-controls-title">
    <div className="trip-head">
      <div>
        <p className="eyebrow">VEHICLE CONTROL</p>
        <h3 id="vehicle-controls-title">Mobility Actions</h3>
      </div>
      <div className="control-state">
        <span className="status"><i />{ignitionOn ? 'READY' : 'STANDBY'}</span>
        <button
          className={`ignition-toggle ${ignitionOn ? 'on' : 'off'}`}
          type="button"
          onClick={() => setIgnitionOn((isOn) => !isOn)}
          aria-pressed={ignitionOn}
        ><Icon name="power" size={13} />IGNITION {ignitionOn ? 'ON' : 'OFF'}</button>
      </div>
    </div>

    <div className="mobility-actions" role="group" aria-label="Mobility state">
      {mobilityOptions.map((option) => <button
        key={option.key}
        type="button"
        className={`mobility-button ${option.className} ${mobility === option.key ? 'active' : ''}`}
        aria-pressed={mobility === option.key}
        disabled={!ignitionOn && option.key === 'MOBILE'}
        onClick={() => setMobility(option.key)}
      >{option.label}</button>)}
    </div>

    <p className="control-feedback">CURRENT STATE: <b>{state}</b></p>

    <div className="driver-capture">
      <div className="driver-image">
        <img src={driverFrame} alt="Latest driver-facing camera frame" />
        <span><i />LIVE FRAME</span>
      </div>
      <div className="driver-details">
        <p className="eyebrow">DRIVER MONITORING</p>
        <h4>{car.driver}</h4>
        <p>Cabin frame verified · {car.id} · {car.place}</p>
        <div className="driver-actions">
          <button type="button" onClick={() => setShowDriverFrame(true)}><Icon name="camera" size={14} />VIEW FACE</button>
          <a href={driverFrame} download="driver-current-frame.svg"><Icon name="download" size={14} />DOWNLOAD FRAME</a>
        </div>
      </div>
    </div>

    {showDriverFrame && <div className="face-modal" role="dialog" aria-modal="true" aria-label="Driver face camera frame">
      <button type="button" aria-label="Close driver frame" onClick={() => setShowDriverFrame(false)}>×</button>
      <img src={driverFrame} alt="Expanded driver-facing camera frame" />
    </div>}
  </section>
}
