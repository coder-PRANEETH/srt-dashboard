import { useEffect, useState } from 'react'
import Icon from './Icon'
import SystemInfo from './SystemInfo'
import driverFrame from '../assets/driver-current-frame.svg'
import './MobilityCard.css'

const mobilityOptions = [
  { key: 'MOBILE', label: 'MOBILE', className: 'mobile' },
  { key: 'IMMOBILE', label: 'IMMOBILISE', className: 'immobile' },
  { key: 'FORCE IMMOBILISE', label: 'FORCE IMMOBILISE', className: 'force' },
]

export default function MobilityCard({ car }) {
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

  return <section className="mobility-card panel" aria-labelledby="vehicle-controls-title">
    <div className="mobility-head">
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
        ><Icon name="power" size={15} />IGNITION {ignitionOn ? 'ON' : 'OFF'}</button>
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

    <div className="mobility-extras">
      <button className="view-face" type="button" onClick={() => setShowDriverFrame(true)}>
        <Icon name="camera" size={16} />VIEW FACE
      </button>
      <SystemInfo car={car} />
    </div>

    <p className="control-feedback">CURRENT STATE: <b>{state}</b></p>

    {showDriverFrame && <div
      className="face-modal"
      role="dialog"
      aria-modal="true"
      aria-label="Driver face camera frame"
      onClick={() => setShowDriverFrame(false)}
    >
      <button type="button" aria-label="Close driver frame" onClick={() => setShowDriverFrame(false)}>×</button>
      <img
        src={driverFrame}
        alt="Expanded driver-facing camera frame"
        onClick={(event) => event.stopPropagation()}
      />
    </div>}
  </section>
}
