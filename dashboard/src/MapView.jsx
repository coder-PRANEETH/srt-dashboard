import { useEffect, useRef } from 'react'
import L from 'leaflet'
import 'leaflet/dist/leaflet.css'

const CENTER = [10.728, 79.0195]
const BOUNDS = [[CENTER[0] - .018, CENTER[1] - .0183], [CENTER[0] + .018, CENTER[1] + .0183]]
export default function MapView({ points, fix, onClose }) {
  const container = useRef(null), layers = useRef(null), dialog = useRef(null)
  useEffect(() => {
    const previous = document.activeElement, overflow = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    dialog.current.querySelector('button').focus()
    const keydown = e => {
      if (e.key === 'Escape') onClose()
      if (e.key !== 'Tab') return
      const controls = [...dialog.current.querySelectorAll('button,a[href],[tabindex="0"]')].filter(e => e.offsetParent !== null)
      const first = controls[0], last = controls.at(-1)
      if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus() }
      if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus() }
    }
    document.addEventListener('keydown', keydown)
    return () => { document.body.style.overflow = overflow; document.removeEventListener('keydown', keydown); previous?.focus() }
  }, [onClose])
  useEffect(() => {
    const map = L.map(container.current, { center: CENTER, zoom: 16, minZoom: 13, maxZoom: 17, maxBounds: BOUNDS, maxBoundsViscosity: 1, zoomControl: true, attributionControl: true })
    map.attributionControl.setPrefix(false)
    L.tileLayer('/tiles/{z}/{x}/{y}.png', { minZoom: 13, maxZoom: 17, bounds: BOUNDS, noWrap: true,
      errorTileUrl: 'data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256"><rect width="256" height="256" fill="%23080604"/></svg>',
      attribution: '© <a href="https://stadiamaps.com/">Stadia Maps</a> © <a href="https://openmaptiles.org/">OpenMapTiles</a> © <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
    }).addTo(map)
    const trail = L.polyline([], { color: '#fff', weight: 3, opacity: .75 }).addTo(map)
    const marker = L.circleMarker(CENTER, { radius: 7, color: '#fff', fillColor: '#ff8a24', fillOpacity: 1, weight: 2 })
    layers.current = { map, trail, marker }
    const observer = new ResizeObserver(() => map.invalidateSize())
    observer.observe(container.current)
    return () => { observer.disconnect(); layers.current = null; map.remove() }
  }, [])
  useEffect(() => {
    const { map, trail, marker } = layers.current
    const segments = [[]]
    for (const p of points) { if (p.gap) segments.push([]); segments.at(-1).push([p.lat, p.lon]) }
    trail.setLatLngs(segments)
    const last = points.at(-1)
    if (last) {
      marker.setLatLng([last.lat, last.lon]).setStyle({ fillOpacity: fix ? 1 : .25 }).addTo(map)
      map.panTo([last.lat, last.lon], { animate: !matchMedia('(prefers-reduced-motion: reduce)').matches })
    }
  }, [points, fix])
  return <div ref={dialog} className="map-overlay" role="dialog" aria-modal="true" aria-label="Fullscreen offline map">
    <header className="topbar"><span className="wordmark">SRT</span><span className="pill">{fix ? 'GPS FIX' : points.length ? 'LAST FIX' : 'NO FIX'}</span><button className="button" onClick={onClose}>CLOSE ×</button></header>
    <div className="map-stage" ref={container} aria-label="Offline map" />
  </div>
}
