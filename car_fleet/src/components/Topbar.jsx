import { useEffect, useState } from 'react'
import './Topbar.css'

const dateFormat = { weekday: 'short', day: '2-digit', month: 'short' }

export default function Topbar() {
  const [now, setNow] = useState(() => new Date())

  useEffect(() => {
    const timer = setInterval(() => setNow(new Date()), 1000)
    return () => clearInterval(timer)
  }, [])

  return <header className="topbar">
    <div className="topbar-upper">
      <div className="brand">
        <div className="brand-mark"><span /><span /><span /></div>
        <span>VOLT<span className="brand-accent">LINE</span></span>
      </div>

      <div className="team-logos" aria-label="SRT Vector and SAEINDIA Southern Section">
        <div className="team-logo srt-logo">
          <i><em /><em /><em /></i>
          <span><b>SRT</b> VECTOR</span>
        </div>
        <span className="team-divider" />
        <div className="team-logo sae-logo">
          <i>SAE</i>
          <span><b>SAEINDIA</b> SOUTHERN SECTION</span>
        </div>
      </div>

      <div className="date-block">
        <b>{now.toLocaleDateString('en-IN', dateFormat).toUpperCase()}</b>
        <span><time dateTime={now.toISOString()}>{now.toLocaleTimeString('en-GB', { hour12: false })}</time> IST</span>
      </div>
    </div>
  </header>
}
