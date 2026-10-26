import { useState } from 'react'
import { dashboardModes } from './modes.js'

export default function ModeStatus({ vehicle, now }) {
  const modes = dashboardModes(vehicle)
  const signature = modes.map(m => m.requested === true ? '1' : m.requested === false ? '0' : '?').join('')
  const [previous, setPrevious] = useState(signature)
  const [notice, setNotice] = useState(null)
  if (previous !== signature) {
    setPrevious(signature)
    const changed = modes.filter((mode, i) => previous[i] === '0' && mode.requested === true)
    if (changed.length) setNotice({ keys: changed.map(mode=>mode.key), expires: now + 3500 })
  }
  return <>
    <section className="mode-strip" aria-label="Vehicle modes and wireless horn">{modes.map(mode =>
      <div key={mode.key} className={`mode-state ${mode.requested ? 'on' : ''} ${mode.state === 'UNAVAILABLE' ? 'unavailable' : ''}`}>
        <span>{mode.label}</span><strong>{mode.state}</strong>
      </div>)}</section>
    {modes.some(mode => mode.key === 'hill' && mode.requested) && <p className="hill-unavailable" role="status">HILL ASSIST UNAVAILABLE · Chassis pitch required · {vehicle?.powertrainReason===10?'pedal blocked at idle':'waiting for powertrain acknowledgement'}</p>}
    {notice && now < notice.expires && <div className="mode-announcement" role="status" aria-live="polite">
      {modes.filter(mode=>notice.keys.includes(mode.key)).map(mode => <div key={mode.key}><strong>{mode.title}</strong>{mode.detail && <span>{mode.detail}</span>}</div>)}
    </div>}
  </>
}
