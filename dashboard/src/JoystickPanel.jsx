import HornButton from './HornButton.jsx'
import MobilityButton from './MobilityButton.jsx'
import { useEffect, useRef, useState } from 'react'
import { createJoystickClient } from './joystickClient.js'
import { joystickCommand } from './joystickModel.js'
import { display } from './diagnostics.js'

export default function JoystickPanel({onClose,onButtons,mobility,onToggleMobility}) {
  const horn=useRef(null),client=useRef(null),pointer=useRef(null),keys=useRef(new Set()),reverse=useRef(false)
  const [state,setState]=useState({ready:false,held:false,message:'CONNECTING'})
  const [point,setPoint]=useState(()=>joystickCommand(0,0)),[limits,setLimits]=useState({throttle:255,steering:255})
  const stop=()=>{pointer.current=null;keys.current.clear();setPoint(joystickCommand(0,0));client.current?.stop()}
  useEffect(()=>{
    const control=createJoystickClient(next=>{
      setState(next)
      if(!next.held){pointer.current=null;keys.current.clear();setPoint(joystickCommand(0,0));reverse.current=next.reverse}
    });client.current=control
    const halt=()=>{pointer.current=null;keys.current.clear();setPoint(joystickCommand(0,0));control.stop()}
    const hidden=()=>{if(document.hidden)halt()}
    const escape=e=>{if(e.key==='Escape'){halt();onClose()}}
    window.addEventListener('blur',halt);window.addEventListener('pagehide',halt);window.addEventListener('offline',halt)
    document.addEventListener('visibilitychange',hidden);document.addEventListener('keydown',escape)
    return ()=>{control.dispose();client.current=null;window.removeEventListener('blur',halt);window.removeEventListener('pagehide',halt);window.removeEventListener('offline',halt);document.removeEventListener('visibilitychange',hidden);document.removeEventListener('keydown',escape)}
  },[onClose])
  useEffect(()=>{if(state.remoteMode===false)client.current?.stop()},[state.remoteMode])
  useEffect(()=>{client.current?.setMobile(mobility.mobile===true&&!mobility.pending)},[mobility.mobile,mobility.pending])
  const command=(x,y)=>{
    const next=joystickCommand(x,y,limits.throttle,limits.steering)
    if(next.throttle)reverse.current=next.reverse
    next.reverse=reverse.current
    return next
  }
  const location=event=>{const b=event.currentTarget.getBoundingClientRect(),r=(b.width-64)/2;return command((event.clientX-b.left-b.width/2)/r,(b.top+b.height/2-event.clientY)/r)}
  const down=event=>{
    event.preventDefault();if(event.button!==0)return
    if(pointer.current!==null||keys.current.size){stop();return}
    const next=location(event)
    if(!client.current?.start(next))return
    pointer.current=event.pointerId;setPoint(next);event.currentTarget.setPointerCapture(event.pointerId)
  }
  const move=event=>{if(pointer.current!==event.pointerId)return;const next=location(event);if(client.current?.update(next))setPoint(next)}
  const up=event=>{if(pointer.current===event.pointerId)stop()}
  const keyboard=event=>{
    if(!['ArrowUp','ArrowDown','ArrowLeft','ArrowRight',' '].includes(event.key))return
    event.preventDefault();if(event.key===' '){stop();return}
    if(event.repeat||pointer.current!==null)return
    keys.current.add(event.key)
    const next=command(Number(keys.current.has('ArrowRight'))-Number(keys.current.has('ArrowLeft')),Number(keys.current.has('ArrowUp'))-Number(keys.current.has('ArrowDown')))
    if(state.held?client.current?.update(next):client.current?.start(next))setPoint(next)
    else keys.current.clear()
  }
  const keyup=event=>{
    if(!keys.current.delete(event.key))return
    event.preventDefault();if(!keys.current.size){stop();return}
    const next=command(Number(keys.current.has('ArrowRight'))-Number(keys.current.has('ArrowLeft')),Number(keys.current.has('ArrowUp'))-Number(keys.current.has('ArrowDown')))
    if(client.current?.update(next))setPoint(next)
  }
  const selectedReverse=state.held?point.reverse:state.reverse
  const throttle=mobility.mobile===true&&!mobility.pending&&!state.throttleBlocked?point.throttle:0
  return <div className="control-page joystick-page">
    <header className="topbar"><span className="wordmark">SRT</span><a className="button" href="/" onClick={()=>{stop();horn.current?.stop()}}>DASHBOARD ↗</a></header>
    <main className="joystick-console" aria-label="Remote control">
      {state.remoteMode===true?<><div className="joystick-heading"><div><span className="caps muted">REMOTE CONTROL</span><h1>Drive by feel.</h1></div><button className="button" onClick={()=>{stop();onButtons()}}>BUTTONS ↗</button></div>
      <div className="joystick-layout"><section className="joystick-station" aria-label="Joystick controls">
        <div className="joystick-direction caps"><span>{state.held?(point.throttle?(point.reverse?'REVERSE':'FORWARD'):'STEERING'):'HOLD & DRAG'}</span><span className="muted">RELEASE TO STOP</span></div>
        <div className={`joystick-pad ${state.held?'held':''} ${state.ready?'ready':''}`} role="application" tabIndex={0} aria-label="Drive joystick" aria-describedby="joystick-instructions" aria-disabled={!state.ready&&!state.held}
          onPointerDown={down} onPointerMove={move} onPointerUp={up} onPointerCancel={up} onLostPointerCapture={up} onContextMenu={event=>event.preventDefault()}
          onKeyDown={keyboard} onKeyUp={keyup} onBlur={()=>{if(keys.current.size)stop()}}
          style={{'--stick-x':`${point.x*100}%`,'--stick-y':`${-point.y*100}%`}}>
          <svg className="joystick-grid" viewBox="0 0 320 320" aria-hidden="true"><circle cx="160" cy="160" r="126"/><circle cx="160" cy="160" r="84"/><circle cx="160" cy="160" r="42"/><path d="M160 22v276M22 160h276M62 62l196 196M62 258 258 62"/><line className="joystick-vector" x1="160" y1="160" x2={160+point.x*128} y2={160-point.y*128}/></svg>
          <span className="compass north">N<span>FORWARD</span></span><span className="compass south">S<span>REVERSE</span></span><span className="compass east">E</span><span className="compass west">W</span>
          <div className="joystick-travel"><span className="joystick-thumb"><i/></span></div>
        </div>
        <HornButton ref={horn}/>
        <p id="joystick-instructions">Drag in any direction. Farther means faster.<br/>Release to stop · Arrow keys work too.</p>
      </section><section className="joystick-telemetry" aria-label="Joystick command values">
        <div className="joystick-signal"><span className="caps">THROTTLE COMMAND</span><div><strong data-testid="throttle-command">{throttle}</strong><span>/ 255</span></div><meter min="0" max="255" value={throttle}/><small>APPLIED PWM <b>{state.appliedThrottlePwm??'—'}</b> · {selectedReverse?'REVERSE':'FORWARD'}</small></div>
        <div className="joystick-signal"><span className="caps">STEERING COMMAND</span><div><strong data-testid="steering-command">{point.steering}</strong><span>/ 255</span></div><meter min="0" max="255" value={point.steering}/><small>{['STOP','LEFT','RIGHT'][point.direction]} · APPLIED PWM <b>{state.steeringPwm??'—'}</b> · ANGLE <b>{display(state.angleDeg,1,'°')}</b></small></div>
        <div className="joystick-limits"><div><label htmlFor="throttle-limit">THROTTLE LIMIT</label><b>{limits.throttle}</b><input id="throttle-limit" type="range" min="0" max="255" value={limits.throttle} disabled={state.held} onChange={e=>setLimits({...limits,throttle:Number(e.target.value)})}/></div><div><label htmlFor="steering-limit">STEERING LIMIT</label><b>{limits.steering}</b><input id="steering-limit" type="range" min="0" max="255" value={limits.steering} disabled={state.held} onChange={e=>setLimits({...limits,steering:Number(e.target.value)})}/></div></div>
      </section></div>
      <div className={`joystick-status ${state.held?'active':''}`} role="status"><i/>{state.message || (state.ready?'READY · PHYSICAL PEDAL BLOCKED WHILE HELD':'CONNECTING')}</div>
      <p className="joystick-footnote">Left/right controls steering motor speed. Releasing stops the motor; it does not center the wheels. Zero throttle keeps the 0.8 V idle signal. Direction changes wait for idle throttle and relay settling.</p></>:<section className="remote-mode-gate" role="status"><span className="caps">{state.remoteMode===false?'REMOTE MODE OFF':'WAITING FOR SWITCH STATUS'}</span><h1>Turn on the physical REMOTE MODE switch</h1><p>Mobile steering, throttle and horn are disabled until Node 3 reports remote mode on.</p></section>}
      <MobilityButton state={mobility} onToggle={onToggleMobility} className="joystick-mobility"/>
    </main>
  </div>
}
