import { useCallback, useMemo, useState } from 'react'
import { fleet, getAlertsFor } from './data/fleet'
import Topbar from './components/Topbar'
import FleetPanel from './components/FleetPanel'
import TelemetryPanel from './components/TelemetryPanel'
import TripCard from './components/TripCard'
import MapPanel from './components/MapPanel'
import AlertPanel from './components/AlertPanel'
import './App.css'

export default function App() {
  const [activeId, setActiveId] = useState(fleet[0].id)

  const activeVehicle = useMemo(
    () => fleet.find((vehicle) => vehicle.id === activeId) ?? fleet[0],
    [activeId],
  )
  const alerts = useMemo(() => getAlertsFor(activeVehicle), [activeVehicle])

  const selectVehicle = useCallback((id) => setActiveId(id), [])

  return <main className="app-shell">
    <Topbar />

    <div className="workspace">
      {/* Fleet selector spans full width — it drives every panel below it */}
      <FleetPanel fleet={fleet} activeId={activeId} onSelect={selectVehicle} />

      <section className="dashboard-grid">
        <div className="dashboard-main">
          <TelemetryPanel car={activeVehicle} alertCount={alerts.length} />
          <TripCard key={activeVehicle.id} car={activeVehicle} />
        </div>
        <div className="dashboard-side">
          <MapPanel car={activeVehicle} />
          <AlertPanel alerts={alerts} car={activeVehicle} />
        </div>
      </section>
    </div>
  </main>
}
