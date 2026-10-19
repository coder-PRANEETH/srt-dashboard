import test from 'node:test'
import assert from 'node:assert/strict'
import { setTimeout as wait } from 'node:timers/promises'
import { createDriveClient } from '../src/driveClient.js'
const reply=data=>({ok:true,json:async()=>data})
const idle=token=>({ready:true,token,held:false,adc:0,reverse:false,canShift:true,message:''})
test('zero level cannot move; late throttle response cannot resume a released hold',async()=>{
  let token=8,complete,latest,ready,moves=0
  const initialized=new Promise(resolve=>ready=resolve)
  const client=createDriveClient(state=>{latest=state;ready()},async(path,options)=>{
    if(options.method==='GET')return reply(idle(token))
    const p=new URLSearchParams(options.body)
    if(p.get('action')==='stop')return reply(idle(++token))
    ++moves;return new Promise(resolve=>complete=()=>resolve(reply({...idle(8),held:true,adc:1023,canShift:false})))
  })
  try {
    await initialized;assert.equal(client.start(0),false);assert.equal(client.start(101),false)
    assert.equal(client.start(25),true);assert.equal(await client.reverse(),false)
    await client.stop();complete();await wait(160)
    assert.equal(latest.held,false);assert.equal(latest.adc,0);assert.equal(latest.token,9);assert.equal(moves,1)
  } finally {client.dispose()}
})
test('failed throttle is not retried after recovery; stale direction response cannot overwrite stop',async()=>{
  let token=1,latest,ready,moves=0,complete
  const initialized=new Promise(resolve=>ready=resolve)
  const client=createDriveClient(state=>{latest=state;ready()},async(path,options)=>{
    if(options.method==='GET')return reply(idle(token))
    const p=new URLSearchParams(options.body)
    if(p.get('action')==='stop')return reply(idle(++token))
    if(p.get('action')==='reverse')return new Promise(resolve=>complete=()=>resolve(reply({...idle(2),reverse:true})))
    ++moves;return {ok:false,status:503}
  })
  try {
    await initialized;assert.equal(client.start(25),true);await wait(350)
    assert.equal(moves,1);assert.equal(latest.held,false)
    const reversing=client.reverse();await client.stop();complete();await reversing;await wait(50)
    assert.equal(latest.reverse,false);assert.equal(latest.token,3)
  } finally {client.dispose()}
})
