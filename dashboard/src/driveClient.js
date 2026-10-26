// One physical hold owns one token. Errors and late responses cannot resume it.
export function createDriveClient(onState, fetcher = fetch) {
  let disposed=false, held=null, changing=false, generation=0, pollTimer, moveTimer, motionRequest, pollRequest, pollFailures=0,throttleEnabled=true
  let state={ready:false,held:false,active:false,adc:0,reverse:false,canShift:false,remoteMode:null,message:'CONNECTING'}
  const publish=patch=>{state={...state,...patch};if(!disposed)onState(state)}
  const apply=data=>({...data,active:data.held,held:held?.action==='throttle',shifting:held?.action==='reverse'||!!data.shifting})
  async function request(method,params,signal,keepalive=false) {
    // Motion stays below the 350 ms firmware lease; status/STOP can wait longer.
    const controller=new AbortController(),abort=()=>controller.abort(),timeout=setTimeout(abort,method==='GET'||params?.action==='stop'?1000:300)
    signal?.addEventListener('abort',abort,{once:true})
    try {
      const response=await fetcher('/api/drive',{method,cache:'no-store',signal:controller.signal,keepalive,
        ...(method==='POST'?{headers:{'X-SRT-Control':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params).toString()}:{})})
      if(!response.ok)throw new Error(response.status===409?'RELEASE & RETRY':'CONTROL UNAVAILABLE')
      const data=await response.json()
      if(typeof data.ready!=='boolean'||!Number.isInteger(data.token)||typeof data.canShift!=='boolean'||typeof data.reverse!=='boolean'||(data.remoteMode!==null&&typeof data.remoteMode!=='boolean')||!Number.isInteger(data.adc)||data.adc<0||data.adc>4095)throw new Error('CONTROL UNAVAILABLE')
      return data
    } finally {clearTimeout(timeout);signal?.removeEventListener('abort',abort)}
  }
  async function stop(message='') {
    const stopped=++generation;pollRequest?.abort();held=null;changing=false;clearTimeout(moveTimer);motionRequest?.abort();pollRequest?.abort()
    publish({ready:false,held:false,active:false,canShift:false,shifting:false,message})
    try {const data=await request('POST',{action:'stop'},null,true);if(generation===stopped)publish({...apply(data),message:message||data.message})}
    catch {if(generation===stopped)publish({ready:false,adc:null,message:'CONNECTION LOST'})}
  }
  async function poll() {
    if(disposed)return
    const before=generation
    if(!held&&!changing) {
      try {pollRequest=new AbortController();const data=await request('GET',null,pollRequest.signal);pollFailures=0;if(before===generation&&!held&&!changing)publish(apply(data))}
      catch {if(before===generation&&++pollFailures>=3)publish({ready:false,canShift:false,adc:null,message:'CONNECTION LOST'})}
    }
    if(!disposed)pollTimer=setTimeout(poll,250)
  }
  async function move(gesture) {
    if(disposed||held!==gesture)return
    const started=Date.now();motionRequest=new AbortController()
    try {
      const data=await request('POST',{action:gesture.action,token:gesture.token,seq:++gesture.seq,...(gesture.action==='reverse'?{reverse:gesture.reverse?1:0}:{percent:gesture.percent})},motionRequest.signal)
      if(disposed||held!==gesture)return
      if(gesture.action==='reverse' && !data.held && data.appliedReverse===gesture.reverse && !data.shifting) {
        held=null;changing=false;publish(apply(data));return
      }
      if(!data.ready||!data.held||data.token!==gesture.token){stop('RELEASE & RETRY');return}
      // Button throttle renews its 2.5 s host lease every 2 s. Node 5 keeps
      // the independent 350 ms node watchdog alive with 50 ms CAN updates.
      const renewMs=gesture.action==='throttle'?2000:80
      publish(apply(data));moveTimer=setTimeout(()=>move(gesture),Math.max(0,renewMs-(Date.now()-started)))
    } catch(error) {if(!disposed&&held===gesture)stop(error.name==='AbortError'?'COMMAND TIMED OUT — RELEASE & RETRY':error.message)}
  }
  poll()
  return {
    start(percent) {
      if(held){stop();return false}
      if(disposed||!throttleEnabled||changing||state.remoteMode!==true||!state.ready||state.active||!Number.isInteger(percent)||percent<1||percent>100)return false
      ++generation;pollRequest?.abort();held={action:'throttle',token:state.token,percent,seq:0}
      publish({held:true,active:true,canShift:false,message:'ARMING'})
      move(held);return true
    },
    async reverse() {
      if(disposed||changing||held||state.remoteMode!==true||state.active||!state.ready||!state.canShift)return false
      ++generation;pollRequest?.abort();changing=true
      held={action:'reverse',token:state.token,reverse:!state.reverse,seq:0}
      publish({shifting:true,active:true,held:false,canShift:false,message:'WAITING FOR DIRECTION ACK'})
      move(held)
      return true
    },
    stop,
    setMobile(enabled){throttleEnabled=enabled;if(!enabled&&held?.action==='throttle')stop()},
    dispose(){if(disposed)return;disposed=true;clearTimeout(pollTimer);stop()},
  }
}
