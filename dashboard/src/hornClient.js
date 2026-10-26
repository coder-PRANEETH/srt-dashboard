// A held button renews an independent horn lease. Failed gestures never resume.
export function createHornClient(onState, fetcher = fetch) {
  let disposed=false, gesture=null, timer, pollTimer, pending, generation=0
  let state={ready:false,held:false,active:null,message:'CONNECTING'}
  const publish=patch=>{state={...state,...patch};if(!disposed)onState(state)}
  async function request(method,params,signal,keepalive=false){
    const abort=new AbortController(),cancel=()=>abort.abort()
    signal?.addEventListener('abort',cancel,{once:true})
    const timeout=setTimeout(cancel,method==='GET'?1000:250)
    try{
      const response=await fetcher('/api/horn',{method,cache:'no-store',signal:abort.signal,keepalive,
        ...(method==='POST'?{headers:{'X-SRT-Control':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params).toString()}: {})})
      if(!response.ok)throw new Error(response.status===409?'RELEASE & RETRY':'HORN UNAVAILABLE')
      const data=await response.json()
      if(typeof data.ready!=='boolean'||!Number.isInteger(data.token)||typeof data.held!=='boolean')throw new Error('HORN UNAVAILABLE')
      return {...data,websiteHeld:data.held,held:false}
    }finally{clearTimeout(timeout);signal?.removeEventListener('abort',cancel)}
  }
  async function stop(message=''){
    const stopped=++generation;gesture=null;clearTimeout(timer);pending?.abort()
    publish({held:false,ready:false,message})
    try{const data=await request('POST',{action:'stop'},null,true);if(stopped===generation&&!gesture)publish({...data,message:message||data.message})}
    catch{if(stopped===generation&&!gesture)publish({ready:false,active:null,message:'HORN CONNECTION LOST'})}
  }
  async function renew(held){
    if(disposed||gesture!==held)return
    const started=Date.now();pending=new AbortController()
    held.sequence=held.sequence%65535+1
    try{
      const data=await request('POST',{action:'press',token:held.token,seq:held.sequence},pending.signal)
      if(disposed||gesture!==held)return
      if(!data.ready||data.token!==held.token){stop('RELEASE & RETRY');return}
      publish({...data,held:true})
      timer=setTimeout(()=>renew(held),Math.max(0,80-(Date.now()-started)))
    }catch{if(!disposed&&gesture===held)stop('HORN CONNECTION LOST')}
  }
  async function poll(){
    const before=generation
    if(!gesture){try{const data=await request('GET');if(before===generation&&!gesture)publish(data)}catch{if(before===generation&&!gesture)publish({ready:false,active:null,message:'HORN CONNECTION LOST'})}}
    if(!disposed)pollTimer=setTimeout(poll,250)
  }
  poll()
  return {
    start(){if(disposed||gesture||!state.ready||state.websiteHeld)return false;generation++;gesture={token:state.token,sequence:0};publish({held:true,message:''});renew(gesture);return true},
    stop,
    dispose(){if(disposed)return;disposed=true;clearTimeout(pollTimer);stop()},
  }
}
