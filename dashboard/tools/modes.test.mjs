import test from 'node:test'
import assert from 'node:assert/strict'
import { dashboardModes } from '../src/modes.js'

test('dashboard shows all six indicators and treats missing data as unknown', () => {
  assert.deepEqual(dashboardModes(null).map(m=>m.key),['cruise','hill','autonomous','remoteMode','horn','reverse'])
  assert.ok(dashboardModes(null).every(m=>m.state==='UNKNOWN'))
  const modes=dashboardModes({horn:false,switches:{cruise:false,hill:false,autonomous:false,remoteMode:false,horn:false,reverse:false}})
  assert.ok(modes.every(m=>m.state==='OFF'))
})
test('hill cannot advertise enabled and applied cruise/reverse must acknowledge activation', () => {
  const vehicle={horn:true,switches:{cruise:true,hill:true,autonomous:true,remoteMode:true,horn:true,reverse:true},powertrainReason:10}
  let modes=dashboardModes(vehicle)
  const hill=modes.find(m=>m.key==='hill')
  assert.equal(hill.state,'UNAVAILABLE');assert.match(hill.title,/UNAVAILABLE/);assert.match(hill.detail,/pedal blocked/)
  assert.equal(modes.find(m=>m.key==='cruise').state,'SELECTED')
  assert.equal(modes.find(m=>m.key==='reverse').state,'REQUESTED')
  assert.equal(modes.find(m=>m.key==='autonomous').state,'REQUESTED')
  assert.equal(modes.find(m=>m.key==='remoteMode').title,'REMOTE MODE ENABLED')
  vehicle.cruiseActive=true;vehicle.reverseActive=true;vehicle.powertrainReason=null
  modes=dashboardModes(vehicle)
  assert.equal(modes.find(m=>m.key==='cruise').state,'ON')
  assert.equal(modes.find(m=>m.key==='reverse').state,'ON')
  assert.match(modes.find(m=>m.key==='hill').detail,/waiting for powertrain acknowledgement/)
})

test('horn indicator follows Node 1 relay command rather than unused Node 3 request',()=>{
  assert.equal(dashboardModes({horn:false,switches:{horn:true}}).find(m=>m.key==='horn').state,'OFF')
  assert.equal(dashboardModes({horn:true,switches:{horn:false}}).find(m=>m.key==='horn').state,'ON')
  assert.equal(dashboardModes({switches:{horn:true}}).find(m=>m.key==='horn').state,'UNKNOWN')
})
