import { useEffect, useRef, useState } from 'react'
import { createSteeringClient } from './steeringClient'
import { createDriveClient } from './driveClient'
import { display } from './diagnostics'

export default function ButtonControls({ onClose, onJoystick }) {
  const control=useRef(null),driveControl=useRef(null),pointers=useRef({steer:null,throttle:null})
  const keyboard=useRef({steer:null,throttle:null})
  const [state,setState]=useState({ready:false,held:0,direction:null,pwm:null,message:'CONNECTING'})
  const [drive,setDrive]=useState({ready:false,held:false,active:false,adc:0,reverse:false,canShift:false,message:'CONNECTING'})
  const [level,setLevel]=useState(100)
  useEffect(()=>{
    const client=createSteeringClient(setState),traction=createDriveClient(setDrive)
    control.current=client;driveControl.current=traction
    const stop=()=>{pointers.current={steer:null,throttle:null};keyboard.current={steer:null,throttle:null};client.stop();traction.stop()}
    const hidden=()=>{if(document.hidden)stop()}
    const keydown=e=>{if(e.key==='Escape'){stop();onClose()}}
    window.addEventListener('blur',stop);window.addEventListener('pagehide',stop);window.addEventListener('offline',stop)
    document.addEventListener('visibilitychange',hidden);document.addEventListener('keydown',keydown)
    return ()=>{
      client.dispose();traction.dispose();control.current=null;driveControl.current=null
      window.removeEventListener('blur',stop);window.removeEventListener('pagehide',stop);window.removeEventListener('offline',stop)
      document.removeEventListener('visibilitychange',hidden);document.removeEventListener('keydown',keydown)
    }
  },[onClose])
  const channelControl=channel=>channel==='steer'?control.current:driveControl.current
  const release=channel=>{pointers.current[channel]=null;keyboard.current[channel]=null;channelControl(channel)?.stop()}
  const releasePointer=(event,channel)=>{if(pointers.current[channel]===event.pointerId)release(channel)}
  const releaseKeyboard=channel=>{if(keyboard.current[channel]!==null)release(channel)}
  const stop=()=>{release('steer');release('throttle')}
  const down=(event,channel,value)=>{
    event.preventDefault()
    if(event.button!==0)return
    if(pointers.current[channel]!==null||keyboard.current[channel]!==null){release(channel);return}
    if(!channelControl(channel)?.start(value))return
    pointers.current[channel]=event.pointerId;event.currentTarget.setPointerCapture(event.pointerId)
  }
  const move=(event,channel)=>{
    if(pointers.current[channel]!==event.pointerId)return
    const box=event.currentTarget.getBoundingClientRect()
    if(event.clientX<box.left||event.clientX>box.right||event.clientY<box.top||event.clientY>box.bottom)release(channel)
  }
  const keys=(event,channel,value)=>{
    if(![' ','Enter'].includes(event.key))return
    event.preventDefault()
    if(event.repeat||pointers.current[channel]!==null||keyboard.current[channel]!==null)return
    if(channelControl(channel)?.start(value))keyboard.current[channel]=event.key
  }
  const keyUp=(event,channel)=>{if(keyboard.current[channel]===event.key){event.preventDefault();release(channel)}}
  const turnButton=(direction,label)=><button className={`steering-turn ${state.held===direction?'pressed':''}`} disabled={!state.held&&(!state.ready||state.direction!==0)}
    aria-label={`Hold ${label.toLowerCase()} to steer`} aria-pressed={state.held===direction} onPointerDown={event=>down(event,'steer',direction)} onPointerUp={event=>releasePointer(event,'steer')} onPointerCancel={event=>releasePointer(event,'steer')}
    onLostPointerCapture={event=>releasePointer(event,'steer')} onPointerMove={event=>move(event,'steer')} onContextMenu={event=>event.preventDefault()}
    onKeyDown={event=>keys(event,'steer',direction)} onKeyUp={event=>keyUp(event,'steer')}
    onBlur={()=>releaseKeyboard('steer')}>
    <span aria-hidden="true">{direction===1?'←':'→'}</span>{label}
  </button>
  return <div className="control-page">
    <header className="topbar"><span className="wordmark">SRT</span><div className="top-actions"><button className="button" onClick={()=>{stop();onJoystick()}}>JOYSTICK ↗</button><a className="button" href="/" onClick={stop}>DASHBOARD ↗</a></div></header>
    <main className="steering-console remote-console" aria-label="Remote control"><div className="remote-heading"><h1>Control</h1><span className="caps muted">HOLD · MAX 5 s</span></div>
      <div className="remote-grid"><section className="remote-steering" aria-label="Steering">
        <div className="remote-label"><span className="caps">STEERING</span><span className="caps muted">PWM 60</span></div>
        <div className="remote-reading"><span>CURRENT ANGLE</span><strong>{state.angleAvailable ? display(state.angleDeg,1,'°') : '—'}</strong></div>
        <div className="steering-controls">{turnButton(1,'LEFT')}{turnButton(2,'RIGHT')}</div>
        <div className="remote-reading"><span>{state.direction===null?'—':['STOP','LEFT','RIGHT'][state.direction]}</span><span role="status">{state.message}</span></div>
      </section><section className="remote-throttle" aria-label="Throttle">
        <div className="remote-label"><label htmlFor="drive-level" className="caps">THROTTLE LEVEL</label><strong>{level}%</strong></div>
        <input id="drive-level" type="range" min="0" max="100" step="5" value={level} disabled={drive.held||drive.active} onChange={event=>setLevel(Number(event.target.value))} aria-valuetext={`${level} percent throttle above idle`} />
        <div className="traction-buttons"><button className={`steering-turn throttle-hold ${drive.held?'pressed':''}`} disabled={!drive.held&&(!drive.ready||drive.active||drive.shifting||level===0)} aria-label="Hold throttle to move" aria-pressed={drive.held} onPointerDown={event=>down(event,'throttle',level)} onPointerUp={event=>releasePointer(event,'throttle')} onPointerCancel={event=>releasePointer(event,'throttle')}
    onLostPointerCapture={event=>releasePointer(event,'throttle')} onPointerMove={event=>move(event,'throttle')} onContextMenu={event=>event.preventDefault()}
    onKeyDown={event=>keys(event,'throttle',level)} onKeyUp={event=>keyUp(event,'throttle')}
    onBlur={()=>releaseKeyboard('throttle')}><span aria-hidden="true">↑</span>THROTTLE</button>
          <button className={`button reverse-select ${drive.reverse?'selected':''}`} disabled={!drive.ready||!drive.canShift||drive.held||drive.active||drive.shifting} aria-pressed={drive.reverse} onClick={()=>driveControl.current?.reverse()}>REVERSE<span>{drive.shifting?'SWITCHING':drive.appliedReverse===null?'—':(drive.appliedReverse??drive.reverse)?'ON':'OFF'}</span></button></div>
        <div className="remote-reading"><span>COMMAND {drive.throttlePwm??0} / 255 · PWM {drive.appliedThrottlePwm??'—'}</span><span role="status">{drive.message}</span></div>
      </section></div>
      <button className="button steering-stop remote-stop" onPointerDown={stop} onClick={event=>{if(event.detail===0)stop()}}>STOP</button>
      <p className="caps muted remote-hint">RELEASE TO IDLE · 0.8 V IDLE SIGNAL · SHIFT ONLY WHEN STATIONARY</p>
    </main>
  </div>
}
