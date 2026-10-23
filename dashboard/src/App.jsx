import HomePage from './HomePage'
import StatusPage from './StatusPage'
import SteeringPanel from './SteeringPanel'
import './App.css'

const goHome = () => window.location.assign('/')

export default function App() {
  const path = window.location.pathname.replace(/\/+$/, '')
  if (path === '/status') return <StatusPage />
  if (path === '/control' || path === '/controls') return <SteeringPanel onClose={goHome} />
  return <HomePage />
}
