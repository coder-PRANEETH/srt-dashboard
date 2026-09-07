import { useCallback, useEffect, useMemo, useState } from 'react'
import { fleet, getAlertsFor } from './data/fleet'
import Topbar from './components/Topbar'
import FleetPanel from './components/FleetPanel'
import TelemetryPanel from './components/TelemetryPanel'
import MobilityCard from './components/MobilityCard'
import MapPanel from './components/MapPanel'
import AlertPanel from './components/AlertPanel'
import './App.css'

export default function App() {
  const [activeId, setActiveId] = useState(fleet[0].id)
  const [fleetOpen, setFleetOpen] = useState(false)
  const [mapOpen, setMapOpen] = useState(false)

  const activeVehicle = useMemo(
    () => fleet.find((vehicle) => vehicle.id === activeId) ?? fleet[0],
    [activeId],
  )
  const alerts = useMemo(() => getAlertsFor(activeVehicle), [activeVehicle])

  /* Picking a vehicle closes the drawer — the choice is made, get out of the way */
  const selectVehicle = useCallback((id) => {
    setActiveId(id)
    setFleetOpen(false)
    setMapOpen(false)
  }, [])

  useEffect(() => {
    if (!fleetOpen) return
    const onKey = (event) => { if (event.key === 'Escape') setFleetOpen(false) }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [fleetOpen])

  return <main className="app-shell">
    <Topbar fleetOpen={fleetOpen} onToggleFleet={() => setFleetOpen((open) => !open)} />

    {/* Fleet selector lives in a drawer so the dashboard keeps the full width */}
    <div
      className={`fleet-backdrop ${fleetOpen ? 'open' : ''}`}
      onClick={() => setFleetOpen(false)}
      aria-hidden="true"
    />
    <aside
      id="fleet-drawer"
      className={`fleet-sidebar ${fleetOpen ? 'open' : ''}`}
      aria-label="Fleet selector"
      aria-hidden={!fleetOpen}
    >
      <FleetPanel
        fleet={fleet}
        activeId={activeId}
        onSelect={selectVehicle}
        onClose={() => setFleetOpen(false)}
      />
    </aside>

    <div className="workspace">
      <section className={`dashboard-grid ${mapOpen ? 'map-open' : ''}`}>
        <div className="dashboard-main">
          <TelemetryPanel car={activeVehicle} alertCount={alerts.length} />
          <MobilityCard key={activeVehicle.id} car={activeVehicle} />
        </div>
        <div className="dashboard-side">
          <MapPanel
            key={`map-${activeVehicle.id}`}
            car={activeVehicle}
            showMap={mapOpen}
            onToggleMap={() => setMapOpen((open) => !open)}
          />
          <AlertPanel alerts={alerts} car={activeVehicle} />
        </div>
      </section>
    </div>
  </main>
}
