import { useState } from 'react'
import ButtonControls from './ButtonControls.jsx'
import JoystickPanel from './JoystickPanel.jsx'

export default function SteeringPanel({onClose}) {
  const [buttons,setButtons]=useState(false)
  return buttons?<ButtonControls onClose={onClose} onJoystick={()=>setButtons(false)}/>:<JoystickPanel onClose={onClose} onButtons={()=>setButtons(true)}/>
}
