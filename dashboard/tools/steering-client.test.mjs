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
test('missing or invalid steering angle prevents a new gesture', async()=>{
  for(const angleDeg of [null,NaN,181]) {
    let settled,latest
    const initialized=new Promise(resolve=>settled=resolve)
    const client=createSteeringClient(state=>{latest=state;settled()},async()=>reply({ready:true,token:1,direction:0,pwm:0,angleDeg}))
    try {await initialized;assert.equal(latest.ready,false);assert.equal(client.start(1),false)}
    finally {client.dispose()}
  }
})
