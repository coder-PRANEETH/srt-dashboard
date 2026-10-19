# Page dependency tree

## / — vehicle dashboard
Entry: dashboard/index.html
- dashboard/src/main.jsx
  - dashboard/src/index.css
  - dashboard/src/App.jsx
    - dashboard/src/App.css
    - leaflet (package)
    - leaflet/dist/leaflet.css (package)
    - local components within App.jsx: Icon, ClusterGauge, CameraView, MapView, CanLog, VehiclePanel
Runtime assets: dashboard/public/tiles/{z}/{x}/{y}.png; standard Vite favicon (not an SRT brand logo). No imported images, custom fonts or shared UI library. API fetches are same-origin. There are no further local imports.

## /status — requested new diagnostics route
No rendered route exists. Reuse CanLog and VehiclePanel after redesign. Firmware route presently returns JSON.
