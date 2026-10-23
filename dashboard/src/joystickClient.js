import { validJoystickCommand } from './joystickModel.js'

export function createJoystickClient(onState,fetcher=fetch) {
  let disposed=false,gesture=null,generation=0,pollTimer,moveTimer,holdTimer,motionRequest,pollRequest
  let state={ready:false,held:false,active:false,token:0,reverse:false,message:'CONNECTING'}
  const publish=patch=>{state={...state,...patch};if(!disposed)onState(state)}
  const apply=data=>({...data,active:data.held,held:!!gesture})
  async function request(method,params,signal,keepalive=false) {
    // Motion stays below the 350 ms firmware lease; status/STOP can wait longer.
    const controller=new AbortController(),abort=()=>controller.abort(),timer=setTimeout(abort,method==='GET'||params?.action==='stop'?1000:300)
    signal?.addEventListener('abort',abort,{once:true})
    try {
      const response=await fetcher('/api/joystick',{method,cache:'no-store',signal:controller.signal,keepalive,
        ...(method==='POST'?{headers:{'X-SRT-Control':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params).toString()}:{})})
      if(!response.ok)throw new Error(response.status===409?'RELEASE & RETRY':'CONTROL UNAVAILABLE')
      const data=await response.json()
      if(typeof data.ready!=='boolean'||typeof data.held!=='boolean'||typeof data.reverse!=='boolean'||!Number.isInteger(data.token))throw new Error('INVALID CONTROL RESPONSE')
      return data
    } finally {clearTimeout(timer);signal?.removeEventListener('abort',abort)}
  }
  async function stop(message='') {
    const before=++generation;pollRequest?.abort();gesture=null;clearTimeout(moveTimer);clearTimeout(holdTimer);motionRequest?.abort();pollRequest?.abort()
    publish({ready:false,held:false,active:false,throttlePwm:0,appliedThrottlePwm:null,steeringPwm:null,message})
    try {const data=await request('POST',{action:'stop'},null,true);if(before===generation)publish({...apply(data),message:message||data.message})}
    catch {if(before===generation)publish({ready:false,appliedThrottlePwm:null,steeringPwm:null,angleDeg:null,message:'CONNECTION LOST'})}
  }
  async function move(current) {
    if(disposed||gesture!==current)return
    const started=Date.now();motionRequest=new AbortController()
    try {
      const {throttle,steering,direction,reverse}=current.command
      const data=await request('POST',{action:'move',token:current.token,seq:++current.seq,throttle,steering,direction,reverse:reverse?1:0},motionRequest.signal)
      if(disposed||gesture!==current)return
      if(!data.ready||!data.held||data.token!==current.token){stop('RELEASE & RETRY');return}
      publish(apply(data))
      moveTimer=setTimeout(()=>move(current),Math.max(0,50-(Date.now()-started)))
    } catch(error) {if(!disposed&&gesture===current)stop(error.name==='AbortError'?'COMMAND TIMED OUT — RELEASE & RETRY':error.message)}
  }
  async function poll() {
    if(disposed)return
    const before=generation
    if(!gesture) {
      try {pollRequest=new AbortController();const data=await request('GET',null,pollRequest.signal);if(before===generation&&!gesture)publish(apply(data))}
      catch {if(before===generation)publish({ready:false,appliedThrottlePwm:null,steeringPwm:null,angleDeg:null,message:'CONNECTION LOST'})}
    }
    if(!disposed)pollTimer=setTimeout(poll,250)
  }
  poll()
  return {
    start(command) {
      if(disposed||gesture||!state.ready||state.active||!validJoystickCommand(command))return false
      ++generation;pollRequest?.abort();gesture={token:state.token,seq:0,command}
      publish({held:true,active:true,message:'TAKING REMOTE CONTROL'})
      holdTimer=setTimeout(()=>stop('RELEASE & RETRY'),4900);move(gesture);return true
    },
    update(command){if(!gesture||!validJoystickCommand(command))return false;gesture.command=command;return true},
    stop,
    dispose(){if(disposed)return;disposed=true;clearTimeout(pollTimer);stop()},
  }
}
