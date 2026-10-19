// One physical hold owns one token. Errors and late responses cannot resume it.
export function createDriveClient(onState, fetcher = fetch) {
  let disposed=false, held=null, changing=false, generation=0, pollTimer, moveTimer, limitTimer, motionRequest
  let state={ready:false,held:false,active:false,adc:0,reverse:false,canShift:false,message:'CONNECTING'}
  const publish=patch=>{state={...state,...patch};if(!disposed)onState(state)}
  const apply=data=>({...data,active:data.held,held:!!held})
  async function request(method,params,signal,keepalive=false) {
    const controller=new AbortController(),abort=()=>controller.abort(),timeout=setTimeout(abort,250)
    signal?.addEventListener('abort',abort,{once:true})
    try {
      const response=await fetcher('/api/drive',{method,cache:'no-store',signal:controller.signal,keepalive,
        ...(method==='POST'?{headers:{'X-SRT-Control':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params).toString()}:{})})
      if(!response.ok)throw new Error(response.status===409?'RELEASE & RETRY':'CONTROL UNAVAILABLE')
      const data=await response.json()
      if(typeof data.ready!=='boolean'||!Number.isInteger(data.token)||typeof data.canShift!=='boolean'||typeof data.reverse!=='boolean'||!Number.isInteger(data.adc)||data.adc<0||data.adc>4095)throw new Error('CONTROL UNAVAILABLE')
      return data
    } finally {clearTimeout(timeout);signal?.removeEventListener('abort',abort)}
  }
  async function stop(message='') {
    const stopped=++generation;held=null;changing=false;clearTimeout(moveTimer);clearTimeout(limitTimer);motionRequest?.abort()
    publish({ready:false,held:false,active:false,canShift:false,shifting:false,message})
    try {const data=await request('POST',{action:'stop'},null,true);if(generation===stopped)publish({...apply(data),message:message||data.message})}
    catch {if(generation===stopped)publish({ready:false,adc:null,message:'CONNECTION LOST'})}
  }
  async function poll() {
    if(disposed)return
    const before=generation
    if(!held&&!changing) {
      try {const data=await request('GET');if(before===generation&&!held&&!changing)publish(apply(data))}
      catch {if(before===generation)publish({ready:false,canShift:false,adc:null,message:'CONNECTION LOST'})}
    }
    if(!disposed)pollTimer=setTimeout(poll,250)
  }
  async function move(gesture) {
    if(disposed||held!==gesture)return
    motionRequest=new AbortController()
    try {
      const data=await request('POST',{action:'throttle',token:gesture.token,seq:++gesture.seq,percent:gesture.percent},motionRequest.signal)
      if(disposed||held!==gesture)return
      if(!data.ready||!data.held||data.token!==gesture.token){stop('RELEASE & RETRY');return}
      publish(apply(data));moveTimer=setTimeout(()=>move(gesture),80)
    } catch(error) {if(!disposed&&held===gesture)stop(error.name==='AbortError'?'CONNECTION LOST':error.message)}
  }
  poll()
  return {
    start(percent) {
      if(held){stop();return false}
      if(disposed||changing||!state.ready||state.active||!Number.isInteger(percent)||percent<1||percent>100)return false
      ++generation;held={token:state.token,percent,seq:0}
      publish({held:true,active:true,canShift:false,message:'ARMING'})
      limitTimer=setTimeout(()=>stop('RELEASE & RETRY'),4900);move(held);return true
    },
    async reverse() {
      if(disposed||changing||held||state.active||!state.ready||!state.canShift)return false
      const before=++generation,token=state.token,reverse=!state.reverse;changing=true
      publish({shifting:true,canShift:false})
      try {
        const data=await request('POST',{action:'reverse',token,reverse:reverse?1:0})
        if(before===generation){changing=false;publish({...apply(data),shifting:false})}
      } catch(error) {if(before===generation)stop(error.message)}
      return true
    },
    stop,
    dispose(){if(disposed)return;disposed=true;clearTimeout(pollTimer);stop()},
  }
}
