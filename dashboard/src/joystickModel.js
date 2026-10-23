export const JOYSTICK_DEADZONE = 0.06
// Screen-independent axes: +x right, +y forward. Circular travel mixes both axes.
export function joystickCommand(x, y, throttleLimit = 255, steeringLimit = 255) {
  if (![x,y,throttleLimit,steeringLimit].every(Number.isFinite)) return joystickCommand(0,0)
  const length=Math.hypot(x,y), radius=Math.min(1,length)
  const travel=radius<=JOYSTICK_DEADZONE?0:(radius-JOYSTICK_DEADZONE)/(1-JOYSTICK_DEADZONE)
  const nx=length?x/length:0,ny=length?y/length:0
  const throttle=Math.round(Math.abs(ny)*travel*Math.max(0,Math.min(255,throttleLimit)))
  const steering=Math.round(Math.abs(nx)*travel*Math.max(0,Math.min(255,steeringLimit)))
  return {x:nx*radius,y:ny*radius,throttle,steering,direction:steering?(x<0?1:2):0,reverse:y<0}
}
export function validJoystickCommand(c) {
  return c && Number.isInteger(c.throttle) && c.throttle>=0 && c.throttle<=255 &&
    Number.isInteger(c.steering) && c.steering>=0 && c.steering<=255 &&
    [0,1,2].includes(c.direction) && (c.direction?c.steering>0:c.steering===0) && typeof c.reverse==='boolean'
}
