import HomePage from './HomePage'
import StatusPage from './StatusPage'
import './App.css'

export default function App() {
  return window.location.pathname.replace(/\/+$/, '') === '/status' ? <StatusPage /> : <HomePage />
}
