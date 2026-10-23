import test from 'node:test'
import assert from 'node:assert/strict'
import { setTimeout as wait } from 'node:timers/promises'
import { createSteeringClient } from '../src/steeringClient.js'
const reply = data => ({ ok:true, json:async()=>({angleAvailable:true,angleDeg:12.3,...data}) })
test('late motion response after release cannot resume or overwrite STOP', async () => {
  let token=10, completeMotion, latest, movements=0, ready
  const initialized = new Promise(resolve=>ready=resolve)
  const fetcher=async(path, options)=>{
    if(options.method==='GET')return reply({ready:true,token,direction:0,pwm:0})
    const params=new URLSearchParams(options.body)
    if(params.get('direction')==='stop')return reply({ready:true,token:++token,direction:0,pwm:0})
    ++movements
    // Deliberately ignore abort to simulate an already completed network write.
    return new Promise(resolve=>{completeMotion=()=>resolve(reply({ready:true,token:10,direction:1,pwm:60}))})
  }
  const client=createSteeringClient(state=>{latest=state;ready()},fetcher)
  try {
    await initialized;assert.ok(client.start(1));await client.stop()
    completeMotion();await wait(150)
    assert.equal(latest.held,0);assert.equal(latest.direction,0);assert.equal(latest.token,11)
    assert.equal(movements,1)
  } finally {client.dispose()}
})
test('failed command stops once and does not restart after healthy polling', async()=>{
  let ready, latest, movements=0, stops=0, token=1
  const initialized=new Promise(resolve=>ready=resolve)
  const fetcher=async(path,options)=>{
    if(options.method==='GET')return reply({ready:true,token,direction:0,pwm:0})
    if(new URLSearchParams(options.body).get('direction')==='stop'){++stops;return reply({ready:true,token:++token,direction:0,pwm:0})}
    ++movements;return {ok:false,status:503}
  }
  const client=createSteeringClient(state=>{latest=state;ready()},fetcher)
  try {await initialized;client.start(2);await wait(350);assert.equal(movements,1);assert.equal(stops,1);assert.equal(latest.held,0)}
  finally {client.dispose()}
})
test('missing or invalid steering angle hides telemetry but permits motor-speed control', async()=>{
  for(const angleDeg of [null,NaN,-270.1,270.1]) {
    let settled,latest
    const initialized=new Promise(resolve=>settled=resolve)
    const client=createSteeringClient(state=>{latest=state;settled()},async()=>reply({ready:true,token:1,direction:0,pwm:0,angleDeg}))
    try {await initialized;assert.equal(latest.ready,true);assert.equal(latest.angleAvailable,false);assert.equal(latest.angleDeg,null);assert.equal(client.start(1),true)}
    finally {client.dispose()}
  }
})
test('steering stays available across the IMU sender range including zero', async()=>{
  for(const angleDeg of [-270,-180.1,0,180.1,270]) {
    let settled,latest
    const initialized=new Promise(resolve=>settled=resolve)
    const client=createSteeringClient(state=>{latest=state;settled()},async()=>reply({ready:true,token:1,direction:0,pwm:0,angleDeg}))
    try {await initialized;assert.equal(latest.ready,true);assert.equal(latest.angleDeg,angleDeg);assert.equal(client.start(1),true)}
    finally {client.dispose()}
  }
})
test('variable applied PWM and a missing angle do not look like a disconnected node',async()=>{
  let latest,settled
  const initialized=new Promise(resolve=>settled=resolve)
  const client=createSteeringClient(state=>{latest=state;settled()},async()=>reply({ready:true,token:1,direction:2,pwm:180,angleAvailable:false,angleDeg:null}))
  try {await initialized;assert.equal(latest.ready,true);assert.equal(latest.pwm,180);assert.equal(latest.angleAvailable,false)}
  finally {client.dispose()}
})
test('disabled controls distinguish CAN startup, missing status, stale status and old firmware',async()=>{
  const cases=[
    [{message:'DASHBOARD CAN NOT READY'},'DASHBOARD CAN NOT READY'],
    [{message:'WAITING FOR NODE 1 CAN STATUS'},'WAITING FOR NODE 1 CAN STATUS'],
    [{message:'NODE 1 CAN STATUS STALE'},'NODE 1 CAN STATUS STALE'],
    [{direction:0,pwm:0,angleAvailable:false,angleDeg:null,ageMs:20},'UPDATE NODE 5 FIRMWARE'],
  ]
  for(const [data,message] of cases) {
    let latest,settled
    const initialized=new Promise(resolve=>settled=resolve)
    const client=createSteeringClient(state=>{latest=state;settled()},async()=>reply({ready:false,token:1,...data}))
    try {await initialized;assert.equal(latest.ready,false);assert.equal(latest.message,message);assert.equal(client.start(1),false)}
    finally {client.dispose()}
  }
})
