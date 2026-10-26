export default function MobilityButton({state,onToggle,className=''}) {
  const mobile=state.mobile===true
  return <div className={`mobility-control ${className}`}>
    <button className={`button mobility-toggle ${mobile?'mobile':'immobile'}`} disabled={!state.ready||state.pending}
      aria-label={mobile?'Set vehicle immobile':'Set vehicle mobile'} aria-pressed={state.mobile===false}
      onPointerDown={event=>{event.preventDefault();if(event.button===0)onToggle()}}
      onClick={event=>{if(event.detail===0)onToggle()}}>{state.mobile===null?'CONNECTING':mobile?'MOBILE':'IMMOBILE'}</button>
    <span className="caps muted" role="status">{state.message}</span>
  </div>
}
