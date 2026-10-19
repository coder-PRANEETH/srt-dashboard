import { useEffect, useRef, useState } from 'react'
import { createSteeringClient } from './steeringClient'
import { createDriveClient } from './driveClient'

export default function SteeringPanel({ onClose }) {
  const control=useRef(null),driveControl=useRef(null),dialog=useRef(null),pointers=useRef({steer:null,throttle:null})
  const [state,setState]=useState({ready:false,held:0,direction:null,pwm:null,message:'CONNECTING'})
  const [drive,setDrive]=useState({ready:false,held:false,active:false,adc:0,reverse:false,canShift:false,message:'CONNECTING'})
  const [level,setLevel]=useState(0)
  useEffect(()=>{
    const client=createSteeringClient(setState),traction=createDriveClient(setDrive)
    control.current=client;driveControl.current=traction
    const previous=document.activeElement,overflow=document.body.style.overflow
    document.body.style.overflow='hidden';dialog.current.querySelector('button').focus()
    const stop=()=>{pointers.current={steer:null,throttle:null};client.stop();traction.stop()}
    const hidden=()=>{if(document.hidden)stop()}
    const keydown=e=>{
      if(e.key==='Escape'){stop();onClose();return}
      if(e.key!=='Tab')return
      const items=[...dialog.current.querySelectorAll('button:not(:disabled),input:not(:disabled)')]
      if(e.shiftKey&&document.activeElement===items[0]){e.preventDefault();items.at(-1).focus()}
      if(!e.shiftKey&&document.activeElement===items.at(-1)){e.preventDefault();items[0].focus()}
    }
    window.addEventListener('blur',stop);window.addEventListener('pagehide',stop);window.addEventListener('offline',stop)
    document.addEventListener('visibilitychange',hidden);document.addEventListener('keydown',keydown)
    return ()=>{
      client.dispose();traction.dispose();control.current=null;driveControl.current=null
      window.removeEventListener('blur',stop);window.removeEventListener('pagehide',stop);window.removeEventListener('offline',stop)
      document.removeEventListener('visibilitychange',hidden);document.removeEventListener('keydown',keydown)
      document.body.style.overflow=overflow;previous?.focus()
    }
  },[onClose])
  const channelControl=channel=>channel==='steer'?control.current:driveControl.current
  const release=channel=>{pointers.current[channel]=null;channelControl(channel)?.stop()}
  const stop=()=>{release('steer');release('throttle')}
  const down=(event,channel,value)=>{
    event.preventDefault()
    if(event.button!==0||pointers.current[channel]!==null){release(channel);return}
    if(!channelControl(channel)?.start(value))return
    pointers.current[channel]=event.pointerId;event.currentTarget.setPointerCapture(event.pointerId)
  }
  const lost=channel=>{if(pointers.current[channel]!==null)release(channel)}
  const move=(event,channel)=>{
    if(pointers.current[channel]!==event.pointerId)return
    const box=event.currentTarget.getBoundingClientRect()
    if(event.clientX<box.left||event.clientX>box.right||event.clientY<box.top||event.clientY>box.bottom)release(channel)
  }
  const keys=(event,channel,value)=>{if([' ','Enter'].includes(event.key)){event.preventDefault();if(!event.repeat)channelControl(channel)?.start(value)}}
  const turnButton=(direction,label)=><button className={`steering-turn ${state.held===direction?'pressed':''}`} disabled={!state.held&&(!state.ready||state.direction!==0)}
    aria-label={`Hold ${label.toLowerCase()} to steer`} aria-pressed={state.held===direction} onPointerDown={event=>down(event,'steer',direction)} onPointerUp={()=>release('steer')} onPointerCancel={()=>release('steer')}
    onLostPointerCapture={()=>lost('steer')} onPointerMove={event=>move(event,'steer')} onContextMenu={event=>event.preventDefault()}
    onKeyDown={event=>keys(event,'steer',direction)} onKeyUp={event=>{if([' ','Enter'].includes(event.key))release('steer')}}
    onBlur={()=>{if(state.held)release('steer')}}>
    <span aria-hidden="true">{direction===1?'←':'→'}</span>{label}
  </button>
  return <div className="steering-overlay" role="dialog" aria-modal="true" aria-label="Remote control" ref={dialog}>
    <header className="topbar"><span className="wordmark">SRT</span><button className="button" onClick={()=>{stop();onClose()}}>CLOSE ×</button></header>
    <section className="steering-console remote-console"><div className="remote-heading"><h1>Control</h1><span className="caps muted">HOLD · MAX 5 s</span></div>
      <div className="remote-grid"><section className="remote-steering" aria-label="Steering">
        <div className="remote-label"><span className="caps">STEERING</span><span className="caps muted">PWM 60</span></div>
        <div className="steering-controls">{turnButton(1,'LEFT')}{turnButton(2,'RIGHT')}</div>
        <div className="remote-reading"><span>{state.direction===null?'—':['STOP','LEFT','RIGHT'][state.direction]}</span><span role="status">{state.message}</span></div>
      </section><section className="remote-throttle" aria-label="Throttle">
        <div className="remote-label"><label htmlFor="drive-level" className="caps">ADC LEVEL</label><strong>{level}%</strong></div>
        <input id="drive-level" type="range" min="0" max="100" step="5" value={level} disabled={drive.held||drive.active} onChange={event=>setLevel(Number(event.target.value))} aria-valuetext={`${level} percent ADC`} />
        <div className="traction-buttons"><button className={`steering-turn throttle-hold ${drive.held?'pressed':''}`} disabled={!drive.held&&(!drive.ready||drive.active||drive.shifting||level===0)} aria-label="Hold throttle to move" aria-pressed={drive.held} onPointerDown={event=>down(event,'throttle',level)} onPointerUp={()=>release('throttle')} onPointerCancel={()=>release('throttle')}
    onLostPointerCapture={()=>lost('throttle')} onPointerMove={event=>move(event,'throttle')} onContextMenu={event=>event.preventDefault()}
    onKeyDown={event=>keys(event,'throttle',level)} onKeyUp={event=>{if([' ','Enter'].includes(event.key))release('throttle')}}
    onBlur={()=>{if(drive.held)release('throttle')}}><span aria-hidden="true">↑</span>THROTTLE</button>
          <button className={`button reverse-select ${drive.reverse?'selected':''}`} disabled={!drive.ready||!drive.canShift||drive.held||drive.active||drive.shifting} aria-pressed={drive.reverse} onClick={()=>driveControl.current?.reverse()}>REVERSE<span>{drive.reverse?'ON':'OFF'}</span></button></div>
        <div className="remote-reading"><span>CAN {drive.adc??'—'} / 4095</span><span role="status">{drive.message}</span></div>
      </section></div>
      <button className="button steering-stop remote-stop" onPointerDown={stop} onClick={event=>{if(event.detail===0)stop()}}>STOP</button>
      <p className="caps muted remote-hint">RELEASE TO STOP · SHIFT ONLY WHEN STATIONARY</p>
    </section>
  </div>
}
