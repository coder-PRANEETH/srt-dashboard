import Icon from './Icon'
import './AlertPanel.css'

export default function AlertPanel({ alerts, car }) {
  return <section className="alert-panel panel" aria-labelledby="alerts-title">
    <div className="alert-head">
      <div>
        <p className="eyebrow">ALERTS RECEIVED</p>
        <h3 id="alerts-title">Vehicle warnings</h3>
      </div>
      <span className={alerts.length ? 'open' : ''}>{String(alerts.length).padStart(2, '0')} OPEN</span>
    </div>

    <div className="alert-list">
      {alerts.length === 0
        ? <p className="alert-empty">All systems nominal for {car.id}. No open warnings.</p>
        : alerts.map((alert) => <article className={`alert-item ${alert.level.toLowerCase()}`} key={alert.id}>
            <Icon name="warning" size={16} />
            <div><b>{alert.title}</b><p>{alert.detail}</p></div>
            <time>{alert.time}</time>
          </article>)}
    </div>
  </section>
}
