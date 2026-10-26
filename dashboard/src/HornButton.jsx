import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { createHornClient } from './hornClient.js'

const HornButton=forwardRef(function HornButton(_,ref){
  const client=useRef(null),pointer=useRef(null),key=useRef(null)
  const [state,setState]=useState({ready:false,held:false,message:'CONNECTING'})
  const stop=()=>{pointer.current=null;key.current=null;client.current?.stop()}
  useImperativeHandle(ref,()=>({stop}))
  useEffect(()=>{
    const control=createHornClient(setState);client.current=control
    const halt=()=>{pointer.current=null;key.current=null;control.stop()}
    const hidden=()=>{if(document.hidden)halt()}
    window.addEventListener('blur',halt);window.addEventListener('pagehide',halt);window.addEventListener('offline',halt)
    document.addEventListener('visibilitychange',hidden)
    return()=>{control.dispose();client.current=null;window.removeEventListener('blur',halt);window.removeEventListener('pagehide',halt);window.removeEventListener('offline',halt);document.removeEventListener('visibilitychange',hidden)}
  },[])
  const release=e=>{if(pointer.current===e.pointerId)stop()}
  return <div className="remote-horn"><button className={`button horn-hold ${state.held?'pressed':''}`} aria-label="Hold horn to sound" aria-pressed={state.held} disabled={!state.held&&(!state.ready||state.websiteHeld)}
    onPointerDown={e=>{e.preventDefault();if(e.button!==0)return;if(pointer.current!==null||key.current!==null){stop();return}if(client.current?.start()){pointer.current=e.pointerId;e.currentTarget.setPointerCapture(e.pointerId)}}}
    onPointerUp={release} onPointerCancel={release} onLostPointerCapture={release}
    onPointerMove={e=>{if(pointer.current!==e.pointerId)return;const b=e.currentTarget.getBoundingClientRect();if(e.clientX<b.left||e.clientX>b.right||e.clientY<b.top||e.clientY>b.bottom)stop()}}
    onKeyDown={e=>{if(![' ','Enter'].includes(e.key))return;e.preventDefault();if(!e.repeat&&key.current===null&&pointer.current===null&&client.current?.start())key.current=e.key}}
    onKeyUp={e=>{if(key.current===e.key){e.preventDefault();stop()}}} onBlur={()=>{if(key.current!==null)stop()}} onContextMenu={e=>e.preventDefault()}>
    HORN · HOLD</button><span className="caps muted" role="status">{state.message||(state.held?'HORN ON':state.wirelessPressed?'STEERING HORN ON':'RELEASE TO SILENCE')}</span></div>
})
export default HornButton
