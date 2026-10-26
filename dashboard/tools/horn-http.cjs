// Local test server for both packed control views.
module.exports=function createHornHttp(remoteMode){
  const state={ready:true,token:30,held:false,active:false,wirelessPressed:false,message:''},commands=[]
  let last=0,fail=false
  return {state,commands,setFail:value=>fail=value,handle(req,res){
    if(new URL(req.url,'http://localhost').pathname!=='/api/horn')return false
    if(state.held&&Date.now()-last>=350){state.held=state.active=false;state.token++}
    state.ready=remoteMode()
    if(req.method==='POST'){
      let body='';req.on('data',c=>body+=c);req.on('end',()=>{
        const command=Object.fromEntries(new URLSearchParams(body));commands.push(command)
        let code=200
        if(command.action==='stop'){state.held=state.active=false;state.token++}
        else if(fail||!state.ready)code=503
        else if(Number(command.token)!==state.token)code=409
        else{state.held=state.active=true;last=Date.now()}
        res.writeHead(code,{'Content-Type':'application/json'});res.end(JSON.stringify(state))
      })
    }else{res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(state))}
    return true
  }}
}
