import { useState } from 'react'
import Icon from './Icon'
import './MapPanel.css'

export default function MapPanel({ car }) {
  const [mapVersion, setMapVersion] = useState(0)
  const [latitude, longitude] = car.position
  const bbox = [longitude - .018, latitude - .012, longitude + .018, latitude + .012]
  const mapSrc = `https://www.openstreetmap.org/export/embed.html?bbox=${bbox.join('%2C')}&layer=mapnik&marker=${latitude}%2C${longitude}`

  return <section className="map-panel panel">
    <div className="map-header">
      <div>
        <p className="eyebrow">REAL-TIME LOCATION</p>
        <h2>New Delhi, India</h2>
      </div>
      <div className="map-header-actions">
        <span className="live-pill"><i />LIVE</span>
        <button className="map-refresh" type="button" onClick={() => setMapVersion((version) => version + 1)}>
          <Icon name="refresh" size={13} />REFRESH
        </button>
      </div>
    </div>

    <div className="map-wrap">
      <iframe
        key={`${car.id}-${mapVersion}`}
        title={`Location of ${car.name} on OpenStreetMap`}
        src={mapSrc}
        loading="lazy"
      />
      <div className="map-scan" />
      <div className="map-marker">
        <div className="marker-halo" />
        <div className="marker-car"><Icon name="car" size={20} /></div>
      </div>
      <div className="map-controls">
        <button type="button" aria-label="Center map"><Icon name="target" size={17} /></button>
        <button type="button" aria-label="Map layers"><Icon name="layers" size={17} /></button>
      </div>
      <div className="map-credit">© OpenStreetMap contributors</div>
    </div>

    <div className="map-footer">
      <div><Icon name="pin" size={17} /><span>Near <b>{car.place}</b> · {car.speed > 0 ? `${car.speed} km/h` : 'Stationary'}</span></div>
      <button type="button">VIEW ROUTE <Icon name="arrow" size={14} /></button>
    </div>
  </section>
}
