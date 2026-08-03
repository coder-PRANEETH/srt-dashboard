import { useEffect, useMemo, useState } from 'react'
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import './App.css'

const initialTelemetry = {
  speed: 67,
  battery: 78,
  range: 248,
  voltage: 382.4,
  current: 84.2,
  batteryTemp: 31.4,
  motorTemp: 54.8,
  controllerTemp: 42.1,
  ambientTemp: 22.7,
  rpm: 3240,
  torque: 186,
  power: 31.8,
  efficiency: 92.4,
  consumption: 16.8,
  regen: 0,
  odometer: 12847.6,
  tripDistance: 36.4,
  tripEnergy: 6.8,
  avgSpeed: 43.2,
  tireFL: 34.6,
  tireFR: 34.3,
  tireRL: 35.1,
  tireRR: 35.0,
  signalStrength: 92,
  latency: 18,
}

const chartSeed = [
  { time: '10:42', temp: 28.2, speed: 42, power: 24.1 },
  { time: '10:43', temp: 28.8, speed: 51, power: 28.3 },
  { time: '10:44', temp: 29.5, speed: 58, power: 30.8 },
  { time: '10:45', temp: 29.9, speed: 62, power: 33.4 },
  { time: '10:46', temp: 30.6, speed: 55, power: 26.9 },
  { time: '10:47', temp: 31.4, speed: 67, power: 31.8 },
]

const driveModes = ['Eco', 'Normal', 'Sport']
const windows = {
  '6m': 6,
  '12m': 12,
  '20m': 20,
}

function makeHealthStatus(value, nominalFloor, warningFloor) {
  if (value >= nominalFloor) {
    return 'Nominal'
  }
  if (value >= warningFloor) {
    return 'Watch'
  }
  return 'Critical'
}

function buildAlerts(telemetry, mode) {
  const alerts = []
  if (telemetry.batteryTemp > 43) {
    alerts.push({ level: 'high', title: 'Battery thermal rise', detail: `Pack temperature reached ${telemetry.batteryTemp.toFixed(1)} degC.` })
  }
  if (telemetry.motorTemp > 68) {
    alerts.push({ level: 'high', title: 'Motor thermal limit nearing', detail: `Motor is now at ${telemetry.motorTemp.toFixed(1)} degC.` })
  }
  if (telemetry.signalStrength < 55) {
    alerts.push({ level: 'medium', title: 'Weak telemetry link', detail: `Signal dropped to ${telemetry.signalStrength.toFixed(0)} percent.` })
  }
  if (telemetry.latency > 120) {
    alerts.push({ level: 'medium', title: 'CAN gateway latency spike', detail: `${telemetry.latency.toFixed(0)} ms observed on latest cycle.` })
  }
  if (mode === 'Sport' && telemetry.consumption > 21) {
    alerts.push({ level: 'low', title: 'High energy draw in Sport', detail: 'Consumption trend is above configured efficiency target.' })
  }
  if (alerts.length === 0) {
    alerts.push({ level: 'ok', title: 'No active alerts', detail: 'All major subsystems are operating within expected range.' })
  }
  return alerts.slice(0, 4)
}

function Icon({ name, size = 18 }) {
  const paths = {
    settings: <><circle cx="12" cy="12" r="3" /><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-1.8 1.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.5v.1h-2.5v-.1a1.7 1.7 0 0 0-1-1.5 1.7 1.7 0 0 0-1.9.3l-.1.1-1.8-1.8.1-.1a1.7 1.7 0 0 0 .3-1.9 1.7 1.7 0 0 0-1.5-1H6.5v-2.5h.1a1.7 1.7 0 0 0 1.5-1 1.7 1.7 0 0 0-.3-1.9l-.1-.1 1.8-1.8.1.1a1.7 1.7 0 0 0 1.9.3 1.7 1.7 0 0 0 1-1.5V4.5H15v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.9-.3l.1-.1 1.8 1.8-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.5 1h.1V13h-.1a1.7 1.7 0 0 0-1.5 1Z" /></>,
    bolt: <path d="m13 2-9 12h7l-1 8 9-12h-7l1-8Z" />,
    battery: <><rect x="3" y="6" width="16" height="12" rx="2" /><path d="M21 10v4M7 10v4M11 10v4M15 10v4" /></>,
    wifi: <><path d="M5 12.5a11 11 0 0 1 14 0M8 16a6.5 6.5 0 0 1 8 0M11 19a2 2 0 0 1 2 0" /><circle cx="12" cy="20" r=".5" /></>,
    gauge: <><path d="M4.9 17a8 8 0 1 1 14.2 0" /><path d="m12 13 3-3M4 20h16" /></>,
    bell: <><path d="M18 8a6 6 0 1 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9" /><path d="M10 20a2 2 0 0 0 4 0" /></>,
  }
  return <svg className="icon" width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[name]}</svg>
}

function TopNavbar({ time, driveMode }) {
  return <header className="topbar">
    <div className="vehicle-mark"><span className="mark"><Icon name="bolt" size={15} /></span><strong>AXION E4</strong></div>
    <div className="topbar-meta">
      <span className="connection"><span className="status-dot" />CONNECTED <small>CAN-FD</small></span>
      <span className="mode-select"><span className="mode-dot" />{driveMode.toUpperCase()}</span>
      <time>{time}</time>
      <button className="icon-button" aria-label="Open settings"><Icon name="settings" /></button>
    </div>
  </header>
}

function CommandStrip({ driveMode, setDriveMode, windowKey, setWindowKey, signalStrength, latency }) {
  return <section className="command-strip">
    <div className="chip-block">
      <span className="chip-label">DRIVE MODE</span>
      <div className="chip-row">{driveModes.map((mode) => <button key={mode} className={`chip ${mode === driveMode ? 'active' : ''}`} onClick={() => setDriveMode(mode)}>{mode.toUpperCase()}</button>)}</div>
    </div>
    <div className="chip-block">
      <span className="chip-label">TIME WINDOW</span>
      <div className="chip-row">{Object.keys(windows).map((key) => <button key={key} className={`chip ${key === windowKey ? 'active' : ''}`} onClick={() => setWindowKey(key)}>{key.toUpperCase()}</button>)}</div>
    </div>
    <div className="network-pill"><Icon name="wifi" size={15} /><div><strong>{signalStrength.toFixed(0)}%</strong><span>{latency.toFixed(0)} ms latency</span></div></div>
  </section>
}

function Panel({ title, action, children, className = '' }) {
  return <section className={`panel ${className}`}><div className="panel-header"><div><h2>{title}</h2></div>{action}</div>{children}</section>
}

function CircularGauge({ value, label = 'SOC' }) {
  const radius = 64
  const circumference = 2 * Math.PI * radius
  return <div className="circular-gauge" style={{ '--progress': `${(value / 100) * circumference}px` }}>
    <svg viewBox="0 0 160 160" aria-label={`${value}% ${label}`}><circle className="gauge-track" cx="80" cy="80" r={radius} /><circle className="gauge-progress" cx="80" cy="80" r={radius} /></svg>
    <div className="gauge-value"><strong>{value}<sup>%</sup></strong><span>{label}</span></div>
  </div>
}

function StatusBadge({ label, status = 'Nominal' }) {
  return <div className="health-row"><span>{label}</span><span className={`badge ${status === 'Nominal' ? 'ok' : status === 'Watch' ? 'warn' : 'danger'}`}><i />{status}</span></div>
}

function TemperatureCard({ label, value, note }) {
  return <div className="temp-card"><div className="temp-icon">°</div><div><span className="metric-label">{label}</span><strong className="temp-value">{value.toFixed(1)}<sup>°C</sup></strong>{note && <span className="metric-note">{note}</span>}</div></div>
}

function MetricCard({ label, value, unit, note, accent = '' }) {
  return <div className="metric-card"><span className="metric-label">{label}</span><strong className={accent}>{value}<sup>{unit}</sup></strong>{note && <span className="metric-note">{note}</span>}</div>
}

function Speedometer({ telemetry, driveMode }) {
  return <Panel title="Vehicle Dynamics" className="speed-panel">
    <div className="speed-readout"><div className="speed-number">{telemetry.speed}<span>km/h</span></div><div className="gear-stack"><span className="active">D</span><span>N</span><span>R</span></div></div>
    <div className="speed-scale"><span>0</span><span>40</span><span>80</span><span>120</span><span>160</span></div>
    <div className="speed-meter"><div className="speed-meter-fill" style={{ width: `${Math.min(100, (telemetry.speed / 160) * 100)}%` }} /></div>
    <div className="mode-line"><span><i className="mode-dot" />DRIVE MODE <strong>{driveMode.toUpperCase()}</strong></span><span>LIMIT <strong>160 km/h</strong></span></div>
    <div className="inline-metrics"><MetricCard label="MOTOR RPM" value={telemetry.rpm.toLocaleString()} unit="rpm" /><MetricCard label="ODOMETER" value={telemetry.odometer.toLocaleString(undefined, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} unit="km" /></div>
  </Panel>
}

function BatteryOverview({ telemetry }) {
  return <Panel title="Battery Overview" className="battery-panel">
    <div className="battery-main"><CircularGauge value={Math.round(telemetry.battery)} /><div className="range-readout"><span className="metric-label">EST. RANGE</span><strong>{telemetry.range}<sup>km</sup></strong><span className="charging"><i />NOT CHARGING</span></div></div>
    <div className="metric-grid"><MetricCard label="PACK VOLTAGE" value={telemetry.voltage.toFixed(1)} unit="V" /><MetricCard label="PACK CURRENT" value={telemetry.current.toFixed(1)} unit="A" accent="blue-text" /></div>
  </Panel>
}

function SystemHealth({ telemetry, driveMode }) {
  const motorStatus = makeHealthStatus(100 - telemetry.motorTemp, 45, 30)
  const batteryStatus = makeHealthStatus(100 - telemetry.batteryTemp, 55, 40)
  const sensorStatus = telemetry.signalStrength > 65 ? 'Nominal' : telemetry.signalStrength > 45 ? 'Watch' : 'Critical'
  return <Panel title="System Health" action={<span className="health-summary"><i />ALL NOMINAL</span>} className="health-panel">
    <div className="temperature-grid"><TemperatureCard label="BATTERY" value={telemetry.batteryTemp} note="within target" /><TemperatureCard label="MOTOR" value={telemetry.motorTemp} /><TemperatureCard label="CONTROLLER" value={telemetry.controllerTemp} /><TemperatureCard label="AMBIENT" value={telemetry.ambientTemp} /></div>
    <div className="status-list"><StatusBadge label="Motor" status={motorStatus} /><StatusBadge label="BMS" status={batteryStatus} /><StatusBadge label="Controller" /><StatusBadge label="Sensors" status={sensorStatus} /><StatusBadge label="Drive Mode" status={driveMode === 'Sport' ? 'Watch' : 'Nominal'} /></div>
  </Panel>
}

function TripKpis({ telemetry }) {
  return <section className="kpi-strip">
    <MetricCard label="TRIP DISTANCE" value={telemetry.tripDistance.toFixed(1)} unit="km" />
    <MetricCard label="TRIP ENERGY" value={telemetry.tripEnergy.toFixed(1)} unit="kWh" />
    <MetricCard label="AVG SPEED" value={telemetry.avgSpeed.toFixed(1)} unit="km/h" />
    <MetricCard label="ENERGY / KM" value={(telemetry.tripEnergy / Math.max(telemetry.tripDistance, 1)).toFixed(2)} unit="kWh" />
  </section>
}

function AlertCenter({ alerts, time }) {
  return <Panel title="Alert Center" action={<Icon name="bell" size={16} />} className="alerts-panel">
    <div className="alerts-list">{alerts.map((alert, index) => <article key={`${alert.title}-${index}`} className={`alert-item ${alert.level}`}>
      <div>
        <h4>{alert.title}</h4>
        <p>{alert.detail}</p>
      </div>
      <time>{time}</time>
    </article>)}</div>
  </Panel>
}

function LineChartCard({ title, data, dataKey, color, unit, domain, windowLabel }) {
  return <Panel title={title} className="chart-panel"><div className="chart-legend"><span><i style={{ background: color }} />LAST {windowLabel}</span><strong>{data[data.length - 1][dataKey]} {unit}</strong></div><div className="chart-wrap"><ResponsiveContainer width="100%" height="100%"><LineChart data={data} margin={{ top: 8, right: 8, left: -24, bottom: 0 }}><CartesianGrid stroke="#2a2320" vertical={false} /><XAxis dataKey="time" tick={{ fill: '#7d746b', fontSize: 10 }} axisLine={false} tickLine={false} /><YAxis domain={domain} tick={{ fill: '#7d746b', fontSize: 10 }} axisLine={false} tickLine={false} /><Tooltip contentStyle={{ background: '#1b1714', border: '1px solid #2a2320', borderRadius: 6, color: '#f5efe9', fontSize: 11 }} /><Line type="monotone" dataKey={dataKey} stroke={color} strokeWidth={2} dot={false} activeDot={{ r: 3, fill: color, stroke: '#0a0a0b', strokeWidth: 2 }} /></LineChart></ResponsiveContainer></div></Panel>
}

function App() {
  const [telemetry, setTelemetry] = useState(initialTelemetry)
  const [now, setNow] = useState(new Date())
  const [history, setHistory] = useState(chartSeed)
  const [driveMode, setDriveMode] = useState('Normal')
  const [windowKey, setWindowKey] = useState('6m')

  useEffect(() => {
    const timer = setInterval(() => {
      const tick = new Date()
      setNow(tick)
      setTelemetry((current) => {
        const modeFactor = driveMode === 'Eco' ? 0.78 : driveMode === 'Sport' ? 1.3 : 1
        const speed = Math.max(0, Math.min(140, current.speed + (Math.random() - 0.48) * (5 + modeFactor * 1.4)))
        const currentDraw = (75 + Math.random() * 16) * modeFactor
        const batteryDrop = 0.005 * modeFactor
        const nextTelemetry = {
          ...current,
          speed: Math.round(speed),
          current: currentDraw,
          battery: Math.max(0, current.battery - batteryDrop),
          range: Math.max(0, Math.round(current.range - 0.015 * modeFactor)),
          batteryTemp: current.batteryTemp + (Math.random() - 0.46) * (0.1 + modeFactor * 0.05),
          motorTemp: current.motorTemp + (Math.random() - 0.44) * (0.18 + modeFactor * 0.12),
          controllerTemp: current.controllerTemp + (Math.random() - 0.5) * 0.12,
          rpm: Math.round(speed * (44 + modeFactor * 6) + (Math.random() - 0.5) * 120),
          power: currentDraw * 0.39,
          consumption: 14 + modeFactor * 3 + Math.random() * 2,
          regen: Math.max(0, (55 - speed) * 0.12),
          odometer: current.odometer + speed / 3600,
          tripDistance: current.tripDistance + speed / 3600,
          tripEnergy: current.tripEnergy + (currentDraw * 0.39) / 3600,
          avgSpeed: current.avgSpeed + (speed - current.avgSpeed) * 0.02,
          tireFL: current.tireFL + (Math.random() - 0.5) * 0.06,
          tireFR: current.tireFR + (Math.random() - 0.5) * 0.06,
          tireRL: current.tireRL + (Math.random() - 0.5) * 0.06,
          tireRR: current.tireRR + (Math.random() - 0.5) * 0.06,
          signalStrength: Math.max(30, Math.min(100, current.signalStrength + (Math.random() - 0.52) * 2.6)),
          latency: Math.max(8, current.latency + (Math.random() - 0.48) * 7.4),
        }
        setHistory((historyItems) => [...historyItems.slice(-19), {
          time: tick.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }),
          temp: Number(nextTelemetry.batteryTemp.toFixed(1)),
          speed: nextTelemetry.speed,
          power: Number(nextTelemetry.power.toFixed(1)),
        }])
        return nextTelemetry
      })
    }, 1000)
    return () => clearInterval(timer)
  }, [driveMode])

  const time = useMemo(() => now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }), [now])
  const chartData = useMemo(() => history.slice(-windows[windowKey]), [history, windowKey])
  const alerts = useMemo(() => buildAlerts(telemetry, driveMode), [telemetry, driveMode])

  return <div className="app-shell"><main>
    <CommandStrip driveMode={driveMode} setDriveMode={setDriveMode} windowKey={windowKey} setWindowKey={setWindowKey} signalStrength={telemetry.signalStrength} latency={telemetry.latency} />
    <TripKpis telemetry={telemetry} />
    <div className="dashboard-grid top-grid"><BatteryOverview telemetry={telemetry} /><Speedometer telemetry={telemetry} driveMode={driveMode} /><SystemHealth telemetry={telemetry} driveMode={driveMode} /></div>
    <div className="section-label"><span>LIVE TELEMETRY</span><span className="update-status"><i />UPDATES EVERY 1 SEC</span></div>
    <div className="dashboard-grid data-grid"><Panel title="Battery" action={<Icon name="battery" size={16} />}><div className="data-card-grid"><MetricCard label="SOC" value={`${telemetry.battery.toFixed(0)}`} unit="%" accent="blue-text" /><MetricCard label="VOLTAGE" value={telemetry.voltage.toFixed(1)} unit="V" /><MetricCard label="CURRENT" value={telemetry.current.toFixed(1)} unit="A" /><MetricCard label="POWER" value={telemetry.power.toFixed(1)} unit="kW" /></div></Panel>
      <Panel title="Motor" action={<Icon name="gauge" size={16} />}><div className="data-card-grid"><MetricCard label="RPM" value={telemetry.rpm.toLocaleString()} unit="rpm" /><MetricCard label="POWER OUTPUT" value={telemetry.power.toFixed(1)} unit="kW" accent="blue-text" /></div></Panel>
      <Panel title="Energy" action={<Icon name="bolt" size={16} />}><div className="data-card-grid"><MetricCard label="INSTANT CONSUMPTION" value={telemetry.consumption.toFixed(1)} unit="kWh/100km" /><MetricCard label="REGEN BRAKING" value={telemetry.regen.toFixed(1)} unit="kW" /><MetricCard label="MODE FACTOR" value={driveMode === 'Eco' ? '0.8' : driveMode === 'Sport' ? '1.3' : '1.0'} unit="x" /></div></Panel>
      <Panel title="Tires" action={<Icon name="gauge" size={16} />}><div className="data-card-grid"><MetricCard label="FRONT LEFT" value={telemetry.tireFL.toFixed(1)} unit="psi" /><MetricCard label="FRONT RIGHT" value={telemetry.tireFR.toFixed(1)} unit="psi" /><MetricCard label="REAR LEFT" value={telemetry.tireRL.toFixed(1)} unit="psi" /><MetricCard label="REAR RIGHT" value={telemetry.tireRR.toFixed(1)} unit="psi" /></div></Panel>
    </div>
    <div className="dashboard-grid support-grid"><AlertCenter alerts={alerts} time={time} /></div>
    <div className="section-label chart-label"><span>RECENT SIGNALS</span><span>ROLLING WINDOW / {windowKey.toUpperCase()}</span></div>
    <div className="dashboard-grid charts-grid"><LineChartCard title="Battery Temperature" data={chartData} dataKey="temp" color="#ff6a00" unit="°C" domain={['dataMin - 2', 'dataMax + 2']} windowLabel={windowKey.toUpperCase()} /><LineChartCard title="Vehicle Speed" data={chartData} dataKey="speed" color="#ff8a3d" unit="km/h" domain={[0, 140]} windowLabel={windowKey.toUpperCase()} /><LineChartCard title="Power Draw" data={chartData} dataKey="power" color="#a24406" unit="kW" domain={['dataMin - 3', 'dataMax + 3']} windowLabel={windowKey.toUpperCase()} /></div>
  </main></div>
}

export default App
