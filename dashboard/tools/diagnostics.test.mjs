import test from 'node:test'
import assert from 'node:assert/strict'
import { decodeFrame, diagnosticsModel, display, filterFrames, parseId } from '../src/diagnostics.js'
import { advanceTrack } from '../src/useTelemetry.js'
const frame = (id, data) => decodeFrame({ seq: 1, ms: 1000, id, data, dlc: data.length })
test('decode actual short payloads, zero and malformed frames', () => {
  assert.match(frame(0x301,[0xd0,7]).text,/2000\/4095 · 48%/)
  assert.match(frame(0x301,[0,0]).text,/0\/4095 · 0%/)
  assert.match(frame(0x300,[0x25]).text,/cruise ON · eco OFF · reverse ON/)
  assert.equal(frame(0x301,[0,16]).issue,true)
  assert.equal(frame(0x301,[0,0,0,0,0,0,0,0]).issue,true)
  assert.equal(frame(0x500,[1,90,0,0,0,0,0,0]).issue,true)
  assert.equal(frame(0x600,[4,0,0,2,0,0,0,0]).issue,true)
  assert.equal(frame(0x400,[90,1,0,79,0,0,1,8]).issue,true)
  assert.equal(display(null),'—');assert.equal(display(0),'0')
})
test('exact hex and combined local filters', () => {
  assert.equal(parseId(' 0X00000301 '),0x301)
  for(const s of ['0x','301junk','20000000','-1'])assert.equal(parseId(s),null)
  assert.equal(parseId(''),undefined)
  const decoded=[frame(0x301,[0xd0,7]),frame(0x300,[0x25]),frame(0x500,[1,90,0,0,0,0,0,0])]
  assert.equal(filterFrames(decoded,{node:'1',query:'d0 07',id:'301'}).length,1)
  assert.equal(filterFrames(decoded,{issues:true})[0].id,0x500)
  assert.equal(filterFrames(decoded,{id:'xyz'}).length,0)
})
test('receive-only nodes stay unknown; stale APIs never report online', () => {
  const feeds={vehicle:{at:1000,data:{canReady:true,nodes:[{id:1,alive:true,ageMs:10},{id:2,alive:true},{id:4,alive:true,ageMs:10,error:2,status:0}],pitchValid:false}}}
  const live=diagnosticsModel(feeds,1500)
  assert.equal(live.nodes[1].state,'UNKNOWN')
  assert.equal(live.nodes[3].state,'FAULT')
  assert.ok(live.faults.some(f=>f.key==='imu'))
  const stale=diagnosticsModel(feeds,5000)
  assert.equal(stale.connected,false)
  assert.ok(stale.nodes.every(n=>n.state==='UNKNOWN'))
})
test('trip rejects jitter/jumps, avoids outage distance, bounds route history', () => {
  let route={points:[],trip:0,heading:null,at:0}
  route=advanceTrack(route,{fix:true,lat:10.728,lon:79.0195},1000)
  const jitter=advanceTrack(route,{fix:true,lat:10.728001,lon:79.0195},2000)
  assert.equal(jitter.trip,0);assert.equal(jitter.points.length,1)
  const jump=advanceTrack(route,{fix:true,lat:11,lon:80},2000)
  assert.equal(jump,route)
  const gap=advanceTrack(route,{fix:true,lat:10.7281,lon:79.0195},20000)
  assert.equal(gap.trip,0);assert.equal(gap.points.at(-1).gap,true)
  for(let n=1;n<700;n++)route=advanceTrack(route,{fix:true,lat:10.728+n*.00004,lon:79.0195},1000+n*1000)
  assert.equal(route.points.length,600)
  assert.ok(route.trip>0);assert.equal(Math.round(route.heading),0)
})
