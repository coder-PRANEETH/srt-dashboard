import test from 'node:test'
import assert from 'node:assert/strict'
import {setTimeout as wait} from 'node:timers/promises'
import {joystickCommand,validJoystickCommand} from '../src/joystickModel.js'
import {createJoystickClient} from '../src/joystickClient.js'
const reply=data=>({ok:true,json:async()=>data})
const idle=token=>({ready:true,held:false,token,reverse:false,remoteMode:true,message:''})
test('one failed joystick status poll keeps the connection indicator stable',async()=>{
  let latest,initialized,gets=0
  const ready=new Promise(resolve=>initialized=resolve),states=[]
  const client=createJoystickClient(state=>{latest=state;states.push(state);initialized()},async(path,options)=>{
    if(options.method!=='GET')return reply(idle(1))
    if(++gets===2)throw new Error('temporary transport error')
    return reply(idle(1))
  })
  try {await ready;await wait(620);assert.ok(gets>=3);assert.equal(latest.ready,true);assert.ok(states.every(state=>state.message!=='CONNECTION LOST'))}
  finally {client.dispose()}
})
test('joystick cannot start when physical remote mode is off',async()=>{
  let latest,initialized,commands=0
  const ready=new Promise(resolve=>initialized=resolve)
  const client=createJoystickClient(state=>{latest=state;initialized()},async(path,options)=>{
    if(options.method==='GET')return reply({...idle(1),ready:false,remoteMode:false,message:'TURN ON REMOTE MODE SWITCH'})
    ++commands;return reply(idle(1))
  })
  try {await ready;assert.equal(latest.remoteMode,false);assert.equal(client.start(joystickCommand(0,1)),false);assert.equal(commands,0)}
  finally {client.dispose()}
})
test('joystick compass directions, circular diagonals, deadzone and speed limits',()=>{
  for(const [x,y,t,s,d,r] of [[0,1,255,0,0,false],[0,-1,255,0,0,true],[1,0,0,255,2,false],[-1,0,0,255,1,false],[1,1,180,180,2,false],[-1,1,180,180,1,false],[1,-1,180,180,2,true],[-1,-1,180,180,1,true],[0,0,0,0,0,false]]){
    const c=joystickCommand(x,y);assert.equal(c.throttle,t);assert.equal(c.steering,s);assert.equal(c.direction,d);assert.equal(c.reverse,r);assert.ok(validJoystickCommand(c))
  }
  assert.equal(joystickCommand(0.03,0.03).throttle,0)
  assert.equal(joystickCommand(1,1,128,60).throttle,91)
  assert.equal(joystickCommand(1,1,128,60).steering,42)
  let prior=0
  for(let n=0;n<=1000;n++){const c=joystickCommand(0,n/1000);assert.ok(c.throttle>=prior&&c.throttle<=255);prior=c.throttle}
  for(let angle=0;angle<360;angle++){const c=joystickCommand(4*Math.cos(angle),4*Math.sin(angle));assert.ok(validJoystickCommand(c));assert.ok(Math.hypot(c.x,c.y)<=1.000001)}
  assert.equal(validJoystickCommand({throttle:256,steering:0,direction:0,reverse:false}),false)
})
test('held joystick streams latest values without overlapping requests; release cannot resume',async()=>{
  let initialized,latest,token=9,finish,inFlight=0,maxFlight=0
  const ready=new Promise(resolve=>initialized=resolve),moves=[]
  const client=createJoystickClient(state=>{latest=state;initialized()},async(path,options)=>{
    assert.equal(path,'/api/joystick')
    if(options.method==='GET')return reply(idle(token))
    const p=new URLSearchParams(options.body)
    if(p.get('action')==='stop')return reply(idle(++token))
    ++inFlight;maxFlight=Math.max(maxFlight,inFlight);moves.push(Object.fromEntries(p))
    if(moves.length===1){await wait(15);--inFlight;return reply({...idle(token),held:true})}
    return new Promise(resolve=>finish=()=>{--inFlight;resolve(reply({...idle(9),held:true,throttlePwm:255}))})
  })
  try{
    await ready;assert.ok(client.start(joystickCommand(0,0)))
    client.update(joystickCommand(1,1));client.update(joystickCommand(0,1))
    await wait(80);assert.equal(moves.length,2);assert.equal(moves[1].throttle,'255');assert.equal(moves[1].steering,'0');assert.equal(maxFlight,1)
    await client.stop();finish();await wait(80)
    assert.equal(latest.held,false);assert.equal(latest.token,10);assert.equal(moves.length,2)
  }finally{client.dispose()}
})
test('joystick failure stops both axes once and never resumes after reconnection',async()=>{
  let initialized,moves=0,stops=0,latest,token=1
  const ready=new Promise(resolve=>initialized=resolve)
  const client=createJoystickClient(state=>{latest=state;initialized()},async(path,options)=>{
    if(options.method==='GET')return reply(idle(token))
    const p=new URLSearchParams(options.body)
    if(p.get('action')==='stop'){++stops;return reply(idle(++token))}
    ++moves;return {ok:false,status:503}
  })
  try{await ready;client.start(joystickCommand(1,1));await wait(350);assert.equal(moves,1);assert.equal(stops,1);assert.equal(latest.held,false)}finally{client.dispose()}
})

test('edge command tolerates a 270 ms acknowledgement without cancelling a live 350 ms lease',async()=>{
  let initialized,latest,token=1,stops=0,moves=0,edgeAck,releaseAck
  const ready=new Promise(resolve=>initialized=resolve),edge=new Promise(resolve=>edgeAck=resolve),released=new Promise(resolve=>releaseAck=resolve)
  const delay=(ms,signal)=>new Promise((resolve,reject)=>{
    const timer=setTimeout(resolve,ms)
    signal.addEventListener('abort',()=>{clearTimeout(timer);reject(new DOMException('Aborted','AbortError'))},{once:true})
  })
  const client=createJoystickClient(state=>{latest=state;initialized();if(state.throttlePwm===180)edgeAck()},async(path,options)=>{
    if(options.method==='GET')return reply(idle(token))
    const p=new URLSearchParams(options.body)
    if(p.get('action')==='stop'){++stops;++token;await delay(400,options.signal);releaseAck();return reply(idle(token))}
    ++moves;await delay(Number(p.get('steering'))?270:5,options.signal)
    return reply({...idle(token),held:true,throttlePwm:Number(p.get('throttle'))})
  })
  try {
    await ready;assert.ok(client.start(joystickCommand(0,1)));await wait(15)
    client.update(joystickCommand(4,4));await edge
    assert.equal(stops,0);assert.equal(latest.held,true);assert.ok(moves>=2)
    await client.stop();await released;assert.equal(latest.held,false);assert.notEqual(latest.message,'CONNECTION LOST')
  } finally {client.dispose()}
})

test('an unacknowledged motion stops both axes and cannot resume from a late reply',async()=>{
  let initialized,latest,token=7,moves=0,stops=0
  const ready=new Promise(resolve=>initialized=resolve)
  const client=createJoystickClient(state=>{latest=state;initialized()},async(path,options)=>{
    if(options.method==='GET')return reply(idle(token))
    const p=new URLSearchParams(options.body)
    if(p.get('action')==='stop'){++stops;return reply(idle(++token))}
    ++moves;await new Promise((resolve,reject)=>options.signal.addEventListener('abort',()=>reject(new DOMException('Aborted','AbortError')),{once:true}))
  })
  try {await ready;client.start(joystickCommand(1,1));await wait(320);assert.equal(stops,1);assert.equal(moves,1);assert.equal(latest.held,false);assert.match(latest.message,/TIMED OUT/);await wait(300);assert.equal(moves,1)}finally{client.dispose()}
})
