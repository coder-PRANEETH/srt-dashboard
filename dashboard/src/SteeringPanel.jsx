import { useEffect, useRef, useState } from 'react'
import { createSteeringClient } from './steeringClient'

export default function SteeringPanel({ onClose }) {
  const control = useRef(null), dialog = useRef(null), pointer = useRef(null)
  const [state, setState] = useState({ ready: false, held: 0, direction: null, pwm: null, message: 'CONNECTING' })
  useEffect(() => {
    const client = createSteeringClient(setState); control.current = client
    const previous = document.activeElement, overflow = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    dialog.current.querySelector('button').focus()
    const stop = () => client.stop()
    const hidden = () => { if (document.hidden) stop() }
    const keydown = e => {
      if (e.key === 'Escape') { stop(); onClose(); return }
      if (e.key !== 'Tab') return
      const buttons = [...dialog.current.querySelectorAll('button:not(:disabled)')]
      if (e.shiftKey && document.activeElement === buttons[0]) { e.preventDefault(); buttons.at(-1).focus() }
      if (!e.shiftKey && document.activeElement === buttons.at(-1)) { e.preventDefault(); buttons[0].focus() }
    }
    window.addEventListener('blur', stop); window.addEventListener('pagehide', stop); window.addEventListener('offline', stop)
    document.addEventListener('visibilitychange', hidden); document.addEventListener('keydown', keydown)
    return () => {
      client.dispose(); control.current = null
      window.removeEventListener('blur', stop); window.removeEventListener('pagehide', stop); window.removeEventListener('offline', stop)
      document.removeEventListener('visibilitychange', hidden); document.removeEventListener('keydown', keydown)
      document.body.style.overflow = overflow; previous?.focus()
    }
  }, [onClose])
  const release = () => { pointer.current = null; control.current?.stop() }
  const down = (event, direction) => {
    event.preventDefault()
    if (event.button !== 0 || !event.isPrimary || pointer.current !== null) { release(); return }
    if (!control.current?.start(direction)) return
    pointer.current = event.pointerId
    event.currentTarget.setPointerCapture(event.pointerId)
  }
  const move = event => {
    if (pointer.current !== event.pointerId) return
    const box = event.currentTarget.getBoundingClientRect()
    if (event.clientX < box.left || event.clientX > box.right || event.clientY < box.top || event.clientY > box.bottom) release()
  }
  const keys = (event, direction) => {
    if (![' ', 'Enter'].includes(event.key)) return
    event.preventDefault()
    if (!event.repeat) control.current?.start(direction)
  }
  const turnButton = (direction, label) => <button className={`steering-turn ${state.held === direction ? 'pressed' : ''}`} disabled={!state.held && (!state.ready || state.direction !== 0)}
    aria-label={`Hold ${label.toLowerCase()} to steer`} aria-pressed={state.held === direction}
    onPointerDown={event => down(event, direction)} onPointerUp={release} onPointerCancel={release}
    onLostPointerCapture={() => { if (pointer.current !== null) release() }} onPointerMove={move}
    onContextMenu={event => event.preventDefault()} onKeyDown={event => keys(event, direction)}
    onKeyUp={event => { if ([' ', 'Enter'].includes(event.key)) release() }} onBlur={() => { if (state.held) release() }}>
    <span aria-hidden="true">{direction === 1 ? '←' : '→'}</span>{label}
  </button>
  return <div className="steering-overlay" role="dialog" aria-modal="true" aria-label="Remote steering" ref={dialog}>
    <header className="topbar"><span className="wordmark">SRT</span><button className="button" onClick={() => { release(); onClose() }}>CLOSE ×</button></header>
    <section className="steering-console"><div className="caps muted">NODE 1 · PWM 60</div><h1>Steering</h1>
      <div className="steering-output"><span className="caps muted">PWM OUTPUT</span><strong>{state.direction === null ? '—' : ['STOP', 'LEFT', 'RIGHT'][state.direction]}</strong></div>
      <div className="steering-controls">{turnButton(1, 'LEFT')}<button className="button steering-stop" onPointerDown={release} onClick={event => { if (event.detail === 0) release() }}>STOP</button>{turnButton(2, 'RIGHT')}</div>
      <p className="caps muted">HOLD TO TURN · RELEASE TO STOP</p><p className="steering-notice" role="status">{state.message || (state.held ? 'HOLDING' : 'MAX 5 s / HOLD')}</p>
    </section>
  </div>
}
