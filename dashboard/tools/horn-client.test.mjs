import test from 'node:test'
import assert from 'node:assert/strict'
import { setTimeout as wait } from 'node:timers/promises'
import { createHornClient } from '../src/hornClient.js'
const reply=data=>({ok:true,json:async()=>data})
test('horn release invalidates a late press response and never resumes',async()=>{
  let settled,latest,complete,presses=0,token=10
  const initialized=new Promise(resolve=>settled=resolve)
  const client=createHornClient(s=>{latest=s;settled()},async(path,o)=>{
    assert.equal(path,'/api/horn')
    if(o.method==='GET')return reply({ready:true,token,held:false,active:false})
    assert.equal(o.headers['X-SRT-Control'],'1')
    if(new URLSearchParams(o.body).get('action')==='stop')return reply({ready:true,token:++token,held:false,active:false})
    presses++;return new Promise(resolve=>{complete=()=>resolve(reply({ready:true,token:10,held:true,active:true}))})
  })
  try{await initialized;assert.ok(client.start());await client.stop();complete();await wait(150);assert.equal(presses,1);assert.equal(latest.held,false);assert.equal(latest.token,11)}finally{client.dispose()}
})
test('horn renews without overlapping commands and stops once after failure',async()=>{
  let settled,latest,presses=0,stops=0,fail=false,token=1,busy=false
  const initialized=new Promise(resolve=>settled=resolve)
  const client=createHornClient(s=>{latest=s;settled()},async(path,o)=>{
    if(o.method==='GET')return reply({ready:true,token,held:false})
    if(new URLSearchParams(o.body).get('action')==='stop'){stops++;return reply({ready:true,token:++token,held:false})}
    assert.equal(busy,false);busy=true;presses++;await wait(20);busy=false
    return fail?{ok:false,status:503}:reply({ready:true,token,held:true})
  })
  try{await initialized;assert.ok(client.start());await wait(270);assert.ok(presses>=3);fail=true;await wait(400);const count=presses;fail=false;await wait(300);assert.equal(presses,count);assert.equal(stops,1);assert.equal(latest.held,false)}finally{client.dispose()}
})
test('horn cannot start without Node 1 status and physical remote mode',async()=>{
  let settled,latest
  const initialized=new Promise(resolve=>settled=resolve)
  const client=createHornClient(s=>{latest=s;settled()},async()=>reply({ready:false,token:1,held:false,message:'TURN ON REMOTE MODE SWITCH'}))
  try{await initialized;assert.equal(client.start(),false);assert.match(latest.message,/REMOTE MODE/)}finally{client.dispose()}
})

test('an existing website horn hold cannot be adopted by another page',async()=>{
  let settled,latest
  const initialized=new Promise(resolve=>settled=resolve)
  const client=createHornClient(s=>{latest=s;settled()},async()=>reply({ready:true,token:1,held:true}))
  try{await initialized;assert.equal(latest.held,false);assert.equal(latest.websiteHeld,true);assert.equal(client.start(),false)}finally{client.dispose()}
})
