// Node 2 owns the latch. Refreshes and switching views read the same state.
export function createMobilityClient(onState,fetcher=fetch) {
  let disposed=false,changing=false,generation=0,timer,pending
  let state={ready:false,mobile:null,pending:false,token:0,message:'WAITING FOR VEHICLE'}
  const publish=patch=>{state={...state,...patch};if(!disposed)onState(state)}
  async function request(params) {
    const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),1000)
    pending=controller
    try {
      const response=await fetcher('/api/mobility',{method:params?'POST':'GET',cache:'no-store',signal:controller.signal,
        ...(params?{headers:{'X-SRT-Control':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params).toString()}: {})})
      if(!response.ok)throw new Error('WAIT FOR VEHICLE & RETRY')
      const data=await response.json()
      if(typeof data.ready!=='boolean'||(data.mobile!==null&&typeof data.mobile!=='boolean')||typeof data.pending!=='boolean'||!Number.isInteger(data.token))throw new Error('WAITING FOR VEHICLE')
      return {...data,message:data.pending?'WAITING FOR VEHICLE ACK':!data.ready?'WAITING FOR VEHICLE':data.mobile?'THROTTLE ENABLED':'THROTTLE AT 0.8 V IDLE'}
    } finally {clearTimeout(timeout);if(pending===controller)pending=null}
  }
  async function poll() {
    const before=generation
    if(!changing) {
      try {const data=await request();if(!disposed&&before===generation)publish(data)}
      catch {if(!disposed&&before===generation)publish({ready:false,message:'VEHICLE CONNECTION LOST'})}
    }
    if(!disposed)timer=setTimeout(poll,250)
  }
  poll()
  return {
    async toggle() {
      if(disposed||changing||state.pending||!state.ready)return false
      const before=++generation,mobile=!state.mobile,token=state.token
      changing=true;pending?.abort();publish({pending:true,message:'WAITING FOR VEHICLE ACK'})
      try {const data=await request({action:mobile?'mobile':'immobile',token});if(!disposed&&generation===before)publish(data);return true}
      catch {if(!disposed&&generation===before)publish({ready:false,pending:false,message:'WAIT FOR VEHICLE & RETRY'});return false}
      finally {changing=false}
    },
    dispose(){disposed=true;++generation;clearTimeout(timer);pending?.abort()},
  }
}
