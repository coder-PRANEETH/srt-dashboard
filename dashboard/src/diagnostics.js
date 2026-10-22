export const number = value => typeof value === 'number' && Number.isFinite(value)
export const display = (value, digits = 0, suffix = '') => number(value) ? `${value.toFixed(digits)}${suffix}` : '—'
export const hex = value => `0x${value.toString(16).toUpperCase().padStart(3, '0')}`
export const onOff = value => value === true ? 'ON' : value === false ? 'OFF' : '—'
export const ageText = ms => number(ms) ? ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(1)} s` : 'No data'
export const NODE_NAMES = ['Pedal / steering', 'Motor control', 'Switch inputs', 'GPS / IMU', 'Dashboard']
export const throttleVoltage = adc => Math.max(0, adc * 3.3 / 4095 - 0.7)
export const throttlePercent = adc => Math.min(100, Math.floor(throttleVoltage(adc) * 100 / 2.6 + 1e-9))
export const SWITCHES = ['cruise', 'eco', 'reverse', 'hill', 'horn', 'headlight', 'autonomous']
export const STEERING_REASONS = ['ready', 'CAN failure', 'command timeout', 'invalid command', 'hold limit', 'steering angle unavailable']
export const POWERTRAIN_REASONS = ['ready', 'CAN unavailable / TX failure', 'PWM setup / write failure', 'pedal missing / stale / invalid', 'switches missing / stale / invalid', 'release pedal to resume', 'direction change / settling', 'traction outputs disabled', 'reverse relays disabled']
export const freshFeed = (feed, now) => feed?.data && now - feed.at < 3500 ? feed.data : null
export function parseId(text) {
  const s = text.trim()
  if (!s) return undefined
  return /^(?:0x)?[0-9a-f]{1,8}$/i.test(s) && parseInt(s, 16) <= 0x1fffffff ? parseInt(s, 16) : null
}
export function decodeFrame(frame) {
  const b = frame.data || [], id = frame.id
  let node = ({ 0x100: 1, 0x110: 5, 0x111: 1, 0x120: 5, 0x121: 1, 0x122: 3, 0x201: 2, 0x301: 1, 0x300: 3, 0x400: 4, 0x401: 4, 0x500: 4 })[id] || null
  if (frame.tx) node = 5
  const bytes = b.map(x => x.toString(16).toUpperCase().padStart(2, '0')).join(' ')
  let kind = 'Unknown frame', text = 'Undecoded', issue = false
  const expected = id === 0x300 ? 1 : id === 0x301 ? 2 : [0x100, 0x110, 0x111, 0x120, 0x121, 0x122, 0x201, 0x400, 0x401, 0x500, 0x600].includes(id) ? 8 : null
  const malformed = b.length !== frame.dlc || b.length > 8 || b.some(x => !Number.isInteger(x) || x < 0 || x > 255) || (expected !== null && frame.dlc !== expected)
  if (malformed) return { ...frame, node, bytes, kind: 'Malformed frame', text: `DLC ${frame.dlc} · expected ${expected ?? '0–8'}`, issue: true }
  const be16 = n => b[n] * 256 + b[n + 1]
  const bit = n => b[0] & (1 << n) ? 'ON' : 'OFF'
  if (id === 0x100) {
    kind='Steering angle'
    const raw=be16(0),angle=(raw>=32768?raw-65536:raw)/10
    const sequence=(b[4]+b[5]*256+b[6]*65536+b[7]*16777216)>>>0
    issue=b[3]!==1 || Math.abs(angle)>180 || b[2]>=250
    text=issue?'Steering angle unavailable':`Steering ${angle.toFixed(1)}° · source age ${b[2]} ms · seq ${sequence}`
  } else if (id === 0x120) {
    kind='Remote handoff'; issue=b[0]>3 || b[1]>1
    text=`${['LOCK','CLAIM','RELEASE SWITCHES','RELEASE PEDAL'][b[0]] || 'INVALID'} · ${b[1]?'REVERSE':'FORWARD'} · seq ${b[6]+256*b[7]}`
  } else if (id === 0x121 || id === 0x122) {
    kind=id===0x121?'Pedal handoff':'Switch handoff'
    const phase=b[0]&127,raw=b[1]+256*b[2]
    issue=b[3]!==0xD1 || phase>2 || phase===2 || raw>(id===0x121?4095:127)
    text=`${['PHYSICAL','REMOTE','ZERO LOCK'][phase] || 'INVALID'} · ${b[0]&128?'REVERSE':'FORWARD'} · ${id===0x121?`pedal ADC ${raw}`:`switch mask ${hex(raw)}`}`
  } else if (id === 0x110 || id === 0x111) {
    kind = id === 0x110 ? 'Steering command' : 'Steering output'
    issue = b[0] > 2 || b[1] !== (b[0] ? 60 : 0) || (id === 0x111 && (b[3] !== 1 || b[2] !== 0))
    text = `${['STOP', 'LEFT', 'RIGHT'][b[0]] || 'INVALID'} · PWM ${b[1]}${id === 0x111 ? ` · ${STEERING_REASONS[b[2]] || 'unknown fault'}` : ''}`
  } else if (id === 0x201) {
    kind = 'Motor outputs'; issue = [0,1,2,4,5,6].some(n=>b[n]>1) || b[3] > 100 || !!b[7]
    text = `Motor 1 ${b[0] ? 'ON' : 'OFF'} · motor 2 ${b[1] ? 'ON' : 'OFF'} · ${b[2] ? 'REVERSE' : 'FORWARD'} · throttle ${b[3]}% · ECO ${b[5] ? 'ON' : 'OFF'} · ${POWERTRAIN_REASONS[b[7]] || `unknown stop reason ${b[7]}`}`
  } else if (id === 0x301) {
    kind = frame.tx ? 'Remote throttle command' : 'Accelerator input'
    const adc = b[0] + b[1] * 256
    issue = adc > 4095
    text = issue ? `ADC ${adc} · out of range` : `ADC ${adc}/4095 · ${throttlePercent(adc)}% · ~${throttleVoltage(adc).toFixed(3)} V corrected`
  } else if (id === 0x300) {
    kind = frame.tx ? 'Remote switch command' : 'Switch inputs'; issue=!!(b[0]&0x80); text = SWITCHES.map((name, n) => `${name} ${bit(n)}`).join(' · ')
  } else if (id === 0x400) {
    kind = 'GPS position'
    const valid = b[0] <= 90 && b[3] <= 180 && be16(1) < 6000 && be16(4) < 6000 && b[0] + be16(1)/6000 <= 90 && b[3] + be16(4)/6000 <= 180
    issue = b[6]!==1 || !valid
    text = !b[6] ? `No fix · ${b[7]} satellites` : b[6]!==1 || !valid ? 'Invalid coordinates' : `${(b[0] + be16(1)/6000).toFixed(6)}, ${(b[3] + be16(4)/6000).toFixed(6)} · ${b[7]} satellites`
  } else if (id === 0x401) {
    kind = 'Ignition'; issue=b[0]>1; text = `${b[0] ? 'ON' : 'OFF'} · ${be16(1)} s`
  } else if (id === 0x500) {
    kind = 'Pitch / hill request'; issue = b[0]>1 || b[2]>100 || b[3]!==1 || b[1] > 180
    text = `${issue ? 'Pitch unavailable' : `Pitch ${b[1] - 90}°`} · hill ${b[0] ? 'ON' : 'OFF'} · hold ${b[2]}%`
  } else if (id === 0x600) {
    node = b[0] >= 1 && b[0] <= 5 ? b[0] : null
    kind = 'Node heartbeat'; issue = !node || b[1]!==1 || !!b[3]
    text = `${!node ? 'Invalid node' : node === 2 ? POWERTRAIN_REASONS[b[3]] || 'Unknown motor fault' : b[3] === 2 && node === 4 ? 'IMU unavailable' : b[3] === 1 ? 'CAN unavailable' : b[1] ? 'Ready' : 'Not ready'} · error ${b[3]} · TX ${b[4]} · RX ${b[5]}`
  }
  return { ...frame, node, bytes, kind, text, issue }
}
export function filterFrames(frames, { id = '', node = '', query = '', issues = false }) {
  const parsed = parseId(id), q = query.trim().toLowerCase()
  return frames.filter(f => parsed !== null && (parsed === undefined || parsed === f.id) && (!node || f.node === Number(node)) && (!issues || f.issue) && (!q || `${hex(f.id)} ${f.kind} ${f.text} ${f.bytes}`.toLowerCase().includes(q)))
}
// Every sensor is derived from received data; older receive-only Node 2 stays unknown.
export function diagnosticsModel(feeds, now) {
  const v = freshFeed(feeds.vehicle, now), g = freshFeed(feeds.gps, now), d = freshFeed(feeds.diagnostics, now)
  const signal = id => {
    const s = d?.signals?.find(s => s.id === id)
    // Firmware freshness is authoritative for each snapshot. Poll latency must
    // not make a healthy 50 ms sender blink offline between HTTP responses.
    return s || null
  }
  const valid = (id, value) => value !== null && value !== undefined && (signal(id)?.fresh ?? !!v)
  const sensor = (name, value, state, detail = '') => ({ name, value, state, detail })
  const state = (id, value) => valid(id, value) ? 'LIVE' : 'UNAVAILABLE'
  const detail = id => { const s = signal(id); return `${hex(id)} · ${ageText(s?.ageMs)}${s ? ` · ${s.invalid} invalid` : ''}` }
  const nodes = NODE_NAMES.map((name, index) => {
    const id = index + 1, n = v?.nodes?.find(n => n.id === id)
    const limit = id === 1 ? 501 : id === 3 ? 3001 : 3000
    const live = n?.alive === true && (id === 5 || !number(n.ageMs) || n.ageMs < limit)
    const noFeedback = id === 2 && n?.source !== 'heartbeat'
    const status = noFeedback || !v ? 'UNKNOWN' : !live ? 'OFFLINE' : n.error || n.status === 0 ? 'FAULT' : id === 5 ? 'LOCAL' : 'LIVE'
    return { id, name, state: status, age: noFeedback ? 'No feedback' : id === 5 && v ? 'Now' : ageText(number(n?.ageMs) ? n.ageMs : null), error: n?.error, sensors: [] }
  })
  nodes[0].sensors = [sensor('Pedal ADC', valid(0x301, v?.acceleratorAdc) ? `${v.acceleratorAdc} / 4095` : '—', state(0x301, v?.acceleratorAdc), `GPIO34 · ${detail(0x301)}`), sensor('Throttle', valid(0x301, v?.acceleratorPct) ? display(v.acceleratorPct, 0, '%') : '—', state(0x301, v?.acceleratorPct)), sensor('Corrected voltage', valid(0x301, v?.acceleratorVoltageEstimate) ? display(v.acceleratorVoltageEstimate, 3, ' V') : '—', valid(0x301, v?.acceleratorVoltageEstimate) ? 'ESTIMATE' : 'UNAVAILABLE', 'Raw − 0.7 V'), sensor('Raw pin voltage', valid(0x301, v?.acceleratorRawVoltageEstimate) ? display(v.acceleratorRawVoltageEstimate, 3, ' V') : '—', 'ESTIMATE')]
  const steering = v?.steering
  nodes[0].sensors.push(sensor('Steering PWM output', steering?.available ? `${['STOP', 'LEFT', 'RIGHT'][steering.direction] || '—'} · PWM ${steering.pwm}` : '—', steering?.available ? steering.reason ? 'FAULT' : 'LIVE' : 'UNAVAILABLE', `LPWM32 / RPWM33 · ${detail(0x111)}${steering?.reason ? ` · ${STEERING_REASONS[steering.reason] || 'fault'}` : ''}`))
  const angleLive = v?.steeringLink === true && valid(0x100,v?.steeringDeg) && number(v.steeringDeg) && Math.abs(v.steeringDeg)<=180
  nodes[0].sensors.push(sensor('Steering angle',angleLive?display(v.steeringDeg,1,'°'):'—',angleLive?'LIVE':'UNAVAILABLE',`Wireless steering IMU · ${detail(0x100)} · source age ${ageText(v?.steeringAgeMs)}`))
  const motorState = value => nodes[1].state === 'UNKNOWN' ? 'NO FEEDBACK' : state(0x201, value)
  nodes[1].sensors = [sensor('Motor PWM command', valid(0x201, v?.throttleOut) ? display(v.throttleOut, 0, '%') : '—', motorState(v?.throttleOut), `GPIO13 / GPIO14 · ${detail(0x201)} · commanded, not measured`), sensor('Reverse relay command', valid(0x201, v?.reverseActive) ? onOff(v.reverseActive) : '—', motorState(v?.reverseActive), `GPIO26 / GPIO33 · ${detail(0x201)} · commanded, not measured`)]
  const powertrainReason = valid(0x201, v?.powertrainReason) ? v.powertrainReason : null
  nodes[1].sensors.push(sensor('Motor stop reason', powertrainReason === null ? '—' : POWERTRAIN_REASONS[powertrainReason] || `unknown reason ${powertrainReason}`, powertrainReason === null ? 'UNAVAILABLE' : powertrainReason ? 'FAULT' : 'LIVE', detail(0x201)))
  const pins = [4, 5, 13, 14, 26, 33, 32]
  nodes[2].sensors = SWITCHES.map((key, i) => sensor(key[0].toUpperCase() + key.slice(1), valid(0x300, v?.switches?.[key]) ? onOff(v.switches[key]) : '—', state(0x300, v?.switches?.[key]), `GPIO${pins[i]} · ${detail(0x300)}`))
  const remote=v?.remoteDrive
  if(remote) {
    const adc=remote.pedalAdc,mask=remote.switchMask
    const pedalLive=valid(0x121,adc),switchLive=valid(0x122,mask)
    nodes[0].sensors.splice(0,4,
      sensor('Pedal ADC',pedalLive?`${adc} / 4095`:'—',pedalLive?'LIVE':'UNAVAILABLE',`GPIO34 · ${detail(0x121)}`),
      sensor('Throttle',pedalLive?`${throttlePercent(adc)}%`:'—',pedalLive?'LIVE':'UNAVAILABLE'),
      sensor('Corrected voltage',pedalLive?display(throttleVoltage(adc),3,' V'):'—',pedalLive?'ESTIMATE':'UNAVAILABLE','Raw − 0.7 V'),
      sensor('Raw pin voltage',pedalLive?display(adc*3.3/4095,3,' V'):'—',pedalLive?'ESTIMATE':'UNAVAILABLE'))
    nodes[2].sensors=SWITCHES.map((key,i)=>sensor(key[0].toUpperCase()+key.slice(1),switchLive?onOff(!!(mask&(1<<i))):'—',switchLive?'LIVE':'UNAVAILABLE',`GPIO${pins[i]} · ${detail(0x122)}`))
    for(const [index,key,id] of [[0,'pedalGate',0x121],[2,'switchGate',0x122]]) {
      const phase=remote[key],live=valid(id,phase)
      nodes[index].sensors.push(sensor('CAN ownership',live?['PHYSICAL','REMOTE','ZERO LOCK'][phase]:'—',live?phase===2?'FAULT':'LIVE':'UNAVAILABLE',detail(id)))
    }
  }
  const gpsLive = g?.fix && valid(0x400, g.lat) && (!number(g.ageMs) || g.ageMs + now - feeds.gps.at < 3000)
  nodes[3].sensors = [sensor('MPU6050 pitch', v?.pitchValid && valid(0x500, v.pitchDeg) ? display(v.pitchDeg, 0, '°') : '—', v?.pitchValid && valid(0x500, v.pitchDeg) ? 'LIVE' : 'UNAVAILABLE', `${detail(0x500)}${nodes[3].error ? ` · error ${nodes[3].error}` : ''}`), sensor('GPS receiver', gpsLive ? `Fix · ${g.sats} satellites` : 'No fix', gpsLive ? 'LIVE' : 'UNAVAILABLE', gpsLive ? `${g.lat.toFixed(6)}, ${g.lon.toFixed(6)} · ${detail(0x400)}` : detail(0x400)), sensor('Ignition', valid(0x401, v?.ignition) ? `${onOff(v.ignition)} · ${display(v.ignitionUptime, 0, ' s')}` : '—', state(0x401, v?.ignition), `GPIO34 · ${detail(0x401)}`), sensor('Hill request', valid(0x500, v?.hillRequest) ? onOff(v.hillRequest) : '—', state(0x500, v?.hillRequest), `${detail(0x500)} · hold ${display(v?.holdPct, 0, '%')}`)]
  const kb = x => display(number(x) ? x / 1024 : null, 1, ' KB')
  nodes[4].sensors = [sensor('CAN controller', v ? v.canReady ? 'Ready' : 'Unavailable' : '—', v?.canReady ? 'LOCAL' : 'UNAVAILABLE', `500 kbps · TX failures ${display(d?.txFailed)}`), sensor('LittleFS', d ? d.fsReady ? 'Ready' : 'Unavailable' : '—', d?.fsReady ? 'LOCAL' : 'UNAVAILABLE', `${kb(d?.fsUsed)} / ${kb(d?.fsTotal)}`), sensor('WiFi clients', display(d?.clients), d ? 'LOCAL' : 'UNAVAILABLE'), sensor('Free heap', kb(d?.freeHeap), d ? 'LOCAL' : 'UNAVAILABLE', `Min ${kb(d?.minFreeHeap)} · largest ${kb(d?.largestFreeBlock)}`), sensor('CAN traffic', d ? `RX ${d.rxTotal} · TX ${d.txTotal}` : '—', d ? 'LOCAL' : 'UNAVAILABLE', `Last RX ${ageText(d?.lastRxAgeMs)} · malformed ${display(d?.malformedFrames)} · unknown ${display(d?.unknownFrames)}`), sensor('Uptime', display(d ? d.uptimeMs/1000 : null, 0, ' s'), d ? 'LOCAL' : 'UNAVAILABLE')]
  if(remote) nodes[4].sensors.push(sensor('Remote CAN command',`${remote.adc} / 4095 · ${remote.reverse?'REVERSE':'FORWARD'}`,remote.reason?'FAULT':'LOCAL',remote.reason || (remote.active?'Held':'Idle')))
  const faults = []
  if (powertrainReason) faults.push({key:'powertrain',label:`Node 2 · ${POWERTRAIN_REASONS[powertrainReason] || `unknown reason ${powertrainReason}`}`,node:2})
  if(remote) {
    if(remote.reason) faults.push({key:'remote-drive',label:`Remote control · ${remote.reason}`,node:5})
    for(const [id,key] of [[1,'pedalGate'],[3,'switchGate']]) if(!number(remote[key]) || remote[key]===2) faults.push({key:`drive-gate-${id}`,label:`Node ${id} handoff ${remote[key]===2?'zero lock':'unavailable'}`,node:id})
  }
  for (const key of ['vehicle', 'gps', 'diagnostics', 'log']) if (!freshFeed(feeds[key], now)) faults.push({ key: `api-${key}`, label: `${key === 'log' ? 'CAN log' : key} API unavailable`, node: 5 })
  for (const n of nodes) {
    if (['OFFLINE', 'FAULT'].includes(n.state)) faults.push({ key: `node-${n.id}`, label: `Node ${n.id} ${n.state.toLowerCase()}${n.error ? ` · error ${n.error}` : ''}`, node: n.id })
  }
  if (v && (!v.pitchValid || !valid(0x500, v.pitchDeg))) faults.push({ key: 'imu', label: 'Node 4 IMU unavailable', node: 4 })
  if (g && !gpsLive) faults.push({ key: 'gps', label: 'Node 4 GPS no fix', node: 4 })
  if (v && (!v.steering?.available || v.steering.reason)) faults.push({ key: 'steering', label: v.steering?.available ? 'Node 1 steering stopped · check output' : 'Node 1 steering unavailable', node: 1 })
  if (v && !angleLive) faults.push({key:'steering-angle',label:'Node 1 wireless steering angle unavailable',node:1})
  if (v && !v.canReady) faults.push({ key: 'can', label: 'CAN unavailable', node: 5 })
  if (d && !d.fsReady) faults.push({ key: 'fs', label: 'LittleFS unavailable', node: 5 })
  return { nodes, faults, connected: !!v, canReady: v?.canReady, diagnostics: d }
}
