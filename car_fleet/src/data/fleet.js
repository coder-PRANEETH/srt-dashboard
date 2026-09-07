export const fleet = [
  { id: 'RV-024', name: 'VOLT XR', type: 'Electric SUV', rangeKm: 238, charge: 86, speed: 64, temp: 27, psi: 34, odometer: '128,452 km', status: 'IN TRANSIT', driver: 'A. Rao', place: 'Rajpath Marg', position: [28.624, 77.218], motor: 'Dual PMSM · 150 kW', torque: '310 Nm', packMaker: 'Exide Neo', packSpec: '72 kWh · 400 V', cells: 'NMC 811 · 96S', ecoRangeKm: 262, powerRangeKm: 191 },
  { id: 'RV-018', name: 'NEON S', type: 'Electric Sedan', rangeKm: 164, charge: 62, speed: 0, temp: 25, psi: 31, odometer: '87,219 km', status: 'PARKED', driver: 'S. Iyer', place: 'Connaught Place', position: [28.614, 77.209], motor: 'Single PMSM · 110 kW', torque: '245 Nm', packMaker: 'Amara Raja', packSpec: '58 kWh · 400 V', cells: 'LFP · 90S', ecoRangeKm: 180, powerRangeKm: 131 },
  { id: 'RV-031', name: 'ORBIT 4', type: 'Electric SUV', rangeKm: 282, charge: 91, speed: 0, temp: 23, psi: 35, odometer: '42,058 km', status: 'CHARGING', driver: 'M. Kaur', place: 'Kashmere Gate Hub', position: [28.633, 77.226], motor: 'Dual PMSM · 165 kW', torque: '340 Nm', packMaker: 'Exide Neo', packSpec: '78 kWh · 400 V', cells: 'NMC 811 · 96S', ecoRangeKm: 310, powerRangeKm: 226 },
  { id: 'RV-007', name: 'ION GT', type: 'Performance EV', rangeKm: 73, charge: 29, speed: 118, temp: 41, psi: 30, odometer: '163,805 km', status: 'IN TRANSIT', driver: 'D. Menon', place: 'Ring Road South', position: [28.602, 77.221], motor: 'Tri-motor · 320 kW', torque: '660 Nm', packMaker: 'LG Chem', packSpec: '84 kWh · 800 V', cells: 'NMC 955 · 108S', ecoRangeKm: 80, powerRangeKm: 58 },
]

export const MAX_SPEED = 180

export const getStatusClass = (status) => status.toLowerCase().replace(/\s+/g, '-')

export const formatRange = (km) => `${km} km`

/** Alerts are derived from live telemetry so they always match the selected vehicle. */
export function getAlertsFor(car) {
  const alerts = []

  if (car.charge <= 30) {
    alerts.push({
      id: 'soc',
      title: 'State of charge critical',
      detail: `Pack at ${car.charge}% · ${car.rangeKm} km remaining`,
      level: 'CRITICAL',
      time: 'just now',
    })
  }
  if (car.temp >= 38) {
    alerts.push({
      id: 'temp',
      title: 'Battery temperature high',
      detail: `Pack running at ${car.temp}°C · thermal limit 45°C`,
      level: 'CRITICAL',
      time: '1 min ago',
    })
  }
  if (car.speed > 110) {
    alerts.push({
      id: 'speed',
      title: 'Overspeed threshold breached',
      detail: `${car.speed} km/h recorded · fleet limit 110 km/h`,
      level: 'WARNING',
      time: '3 min ago',
    })
  }
  if (car.psi < 32) {
    alerts.push({
      id: 'psi',
      title: 'Tyre pressure check due',
      detail: `Rear-left sensor reports ${car.psi} PSI · target 34 PSI`,
      level: 'WARNING',
      time: '12 min ago',
    })
  }
  if (car.status === 'CHARGING') {
    alerts.push({
      id: 'chg',
      title: 'Charging session active',
      detail: `Connected at ${car.place} · DC fast charge`,
      level: 'INFO',
      time: '24 min ago',
    })
  }

  return alerts
}
