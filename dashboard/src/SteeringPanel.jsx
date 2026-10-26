import { useEffect, useRef, useState } from 'react'
import ButtonControls from './ButtonControls.jsx'
import JoystickPanel from './JoystickPanel.jsx'
import { createMobilityClient } from './mobilityClient.js'

export default function SteeringPanel({onClose}) {
  const [buttons,setButtons]=useState(false)
  const [mobility,setMobility]=useState({ready:false,mobile:null,pending:false,message:'WAITING FOR VEHICLE'})
  const client=useRef(null)
  useEffect(()=>{const control=createMobilityClient(setMobility);client.current=control;return()=>{control.dispose();client.current=null}},[])
  const toggleMobility=()=>client.current?.toggle()
  return buttons?<ButtonControls onClose={onClose} onJoystick={()=>setButtons(false)} mobility={mobility} onToggleMobility={toggleMobility}/>:<JoystickPanel onClose={onClose} onButtons={()=>setButtons(true)} mobility={mobility} onToggleMobility={toggleMobility}/>
}
