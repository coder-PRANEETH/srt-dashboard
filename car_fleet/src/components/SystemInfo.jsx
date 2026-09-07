import { useEffect, useState } from 'react'
import Icon from './Icon'
import './SystemInfo.css'

export default function SystemInfo({ car }) {
  const [open, setOpen] = useState(false)

  useEffect(() => {
    if (!open) return
    const onKey = (event) => { if (event.key === 'Escape') setOpen(false) }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open])

  const specs = [
    { group: 'MOTOR', rows: [['Drive unit', car.motor], ['Peak torque', car.torque], ['Top speed', '180 km/h']] },
    { group: 'BATTERY', rows: [['Manufacturer', car.packMaker], ['Rating', car.packSpec], ['Cell chemistry', car.cells]] },
  ]

  return <>
    <button className="system-info-button" type="button" onClick={() => setOpen(true)} aria-haspopup="dialog">
      <Icon name="chip" size={16} />SYSTEM INFO
    </button>

    {open && <div
      className="system-info-modal"
      role="dialog"
      aria-modal="true"
      aria-label={`System information for ${car.name}`}
      onClick={() => setOpen(false)}
    >
      <div className="system-info-sheet" onClick={(event) => event.stopPropagation()}>
        <div className="system-info-head">
          <div>
            <p className="eyebrow">SYSTEM INFORMATION</p>
            <h3>{car.name} <span>{car.id}</span></h3>
          </div>
          <button type="button" aria-label="Close system information" onClick={() => setOpen(false)}>×</button>
        </div>

        <div className="system-info-body">
          {specs.map((spec) => <div className="spec-group" key={spec.group}>
            <p className="spec-group-title">{spec.group}</p>
            <dl>
              {spec.rows.map(([label, value]) => <div key={label}>
                <dt>{label}</dt><dd>{value}</dd>
              </div>)}
            </dl>
          </div>)}
        </div>
      </div>
    </div>}
  </>
}
