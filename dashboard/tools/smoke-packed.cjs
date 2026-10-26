/* Exercises the actual compressed ESP32 bundle. No physical board required. */
const assert = require('node:assert/strict')
const fs = require('node:fs')
const http = require('node:http')
const path = require('node:path')
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright')
const root = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'node5_dash', 'data'))
const vehicle = {
  horn:false,hornInputAvailable:true,hornWirelessPressed:false,
  canReady: true, frames: 42, acceleratorAdc: 2000, acceleratorPct: 32, acceleratorVoltageEstimate: 0.812, acceleratorRawVoltageEstimate: 1.612,
  steering: { available: true, direction: 0, pwm: 0, reason: 0, ageMs: 10 },
  wheelPitchDeg:9,wheelPitchValid:true,wheelPitchForControl:false,hillAvailable:false,
  steeringDeg: -32.5, steeringLink: true, steeringAgeMs: 10, steeringSequence: 100,
  switches: { cruise: true, hill: false, autonomous: true, remoteMode: false, horn: false, reverse: true },
  nodes: [1,2,3,5].map(id => ({ id, alive: id === 2 ? null : true, status: id === 2 ? null : 1, error: 0, ageMs: 10 })),
}
const gps = { source: 'local-uart', receiving: true, rxAgeMs: 10, charsProcessed: 500, passedChecksum: 10, failedChecksum: 0, hdop: 1.2, fix: true, lat: 10.728, lon: 79.0195, sats: 8, speed: 20, ageMs: 20 }
const diagnostics = { uptimeMs: 5000, freeHeap: 140000, minFreeHeap: 130000, largestFreeBlock: 110000, fsReady: true, fsTotal: 1966080, fsUsed: 1500000, clients: 1, canReady: true, rxTotal: 42, txTotal: 5, txFailed: 0, lastRxAgeMs: 20, malformedFrames: 0, unknownFrames: 0, signals: [0x301,0x300].map(id => ({ id, seen: true, fresh: true, ageMs: 20, invalid: 0, timeoutMs: id === 0x301 ? 501 : 3000 })) }
let frames = [
  { seq: 1, ms: 1000, id: 0x301, dlc: 2, data: [0xD0,0x07] },
  { seq: 2, ms: 1000, id: 0x300, dlc: 1, data: [0x25] },
  { seq: 3, ms: 1000, id: 0x201, dlc: 8, data: [0,0,0,24,0,0,0,3] },
]
const calls = {}, fail = new Set()
const steering = {ready:true, token:99, direction:0, pwm:0, reason:0, ageMs:10, angleAvailable:true, angleDeg:-32.5, angleAgeMs:10, angleSequence:100}
const steeringCommands=[]
const drive={ready:true,remoteMode:true,token:100,held:false,adc:0,throttlePwm:0,appliedThrottlePwm:62,appliedReverse:false,shifting:false,reverse:false,canShift:true,message:''}
const joystick={ready:true,remoteMode:true,held:false,token:100,reverse:false,throttlePwm:0,steeringDirection:0,steeringPwm:0,angleDeg:null,appliedThrottlePwm:62,message:''}
const driveCommands=[]
let lastDrive=0,driveStopped=0
function driveExpire() {
  if(drive.held && Date.now()-lastDrive>=(drive.shifting?350:2500)){drive.held=false;drive.adc=0;drive.throttlePwm=0;drive.appliedThrottlePwm=62;++drive.token;driveStopped=Date.now()}
  drive.canShift=!drive.held && Date.now()-driveStopped>=1000
}
let lastMotion=0
function steeringExpire() {
  if(steering.direction && Date.now()-lastMotion>=350){steering.direction=0;steering.pwm=0;++steering.token}
}
const mime = { '.html':'text/html', '.js':'application/javascript', '.css':'text/css', '.png':'image/png', '.svg':'image/svg+xml' }
const hornHttp=require('./horn-http.cjs')(()=>drive.remoteMode)
const server = http.createServer((req,res) => {
  if(hornHttp.handle(req,res))return
  const pathname = new URL(req.url,'http://localhost').pathname
  calls[pathname] = (calls[pathname] || 0)+1
  if(pathname==='/api/drive') {
    driveExpire()
    if(req.method==='POST') {
      let body='';req.on('data',chunk=>body+=chunk);req.on('end',()=>{
        const params=new URLSearchParams(body),action=params.get('action')
        driveCommands.push({action,percent:Number(params.get('percent')),header:req.headers['x-srt-control']})
        let status=200
        if(fail.has(pathname))status=503
        else if(action==='stop'){if(drive.held)driveStopped=Date.now();drive.held=false;drive.adc=0;drive.throttlePwm=0;drive.appliedThrottlePwm=62;++drive.token}
        else if(Number(params.get('token'))!==drive.token)status=409
        else if(action==='reverse'){if(!drive.canShift)status=409;else {drive.reverse=drive.appliedReverse=params.get('reverse')==='1';++drive.token}}
        else {drive.held=true;drive.throttlePwm=Math.round(Number(params.get('percent'))*255/100);drive.appliedThrottlePwm=62+Math.floor(193*drive.throttlePwm/255);lastDrive=Date.now()}
        driveExpire();res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(drive))
      });return
    }
    res.writeHead(fail.has(pathname)?503:200,{'Content-Type':'application/json'});res.end(JSON.stringify(drive));return
  }
  if(pathname==='/api/joystick') {
    const body={...joystick,ready:drive.remoteMode,remoteMode:drive.remoteMode}
    res.writeHead(200,{'Content-Type':'application/json','Cache-Control':'no-store'});res.end(JSON.stringify(body));return
  }
  if(pathname==='/api/steering') {
    steeringExpire()
    if(req.method==='POST') {
      let body=''; req.on('data',chunk=>body+=chunk); req.on('end',()=>{
        const params=new URLSearchParams(body), direction=params.get('direction')
        steeringCommands.push({direction, token:params.get('token'), seq:params.get('seq'), at:Date.now(), header:req.headers['x-srt-control']})
        let status=200
        if(fail.has(pathname))status=503
        else if(direction==='stop'){steering.direction=0;steering.pwm=0;++steering.token}
        else if(Number(params.get('token'))!==steering.token)status=409
        else {steering.direction=direction==='left'?1:2;steering.pwm=60;lastMotion=Date.now()}
        res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(steering))
      });return
    }
    res.writeHead(fail.has(pathname)?503:200,{'Content-Type':'application/json'});res.end(JSON.stringify(steering));return
  }
  const api = { '/api/vehicle':vehicle, '/api/gps':gps, '/api/diagnostics':diagnostics, '/api/can-log':{ ready:true, total:42, uptimeMs:5000, frames } }
  if (Object.hasOwn(api,pathname)) {
    res.writeHead(fail.has(pathname) ? 503 : 200, { 'Content-Type':'application/json', 'Cache-Control':'no-store' }); res.end(JSON.stringify(api[pathname])); return
  }
  const relative = ['/', '/status', '/status/', '/control', '/control/', '/controls', '/controls/'].includes(pathname) ? 'index.html' : pathname.slice(1)
  const target = path.resolve(root,relative), gz = fs.existsSync(target+'.gz'), file = gz ? target+'.gz' : target
  if (!target.startsWith(root+path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404);res.end();return }
  res.writeHead(200,{ 'Content-Type':mime[path.extname(target)] || 'application/octet-stream', ...(gz ? {'Content-Encoding':'gzip'} : {}) });fs.createReadStream(file).pipe(res)
})
;(async () => {
  await new Promise(resolve => server.listen(0,'127.0.0.1',resolve))
  const browser = await chromium.launch({headless:true, executablePath:process.env.BROWSER_PATH, args:['--no-sandbox']})
  try {
    const context = await browser.newContext({viewport:{width:1280,height:800}, hasTouch:true, permissions:['clipboard-read','clipboard-write']})
    const page = await context.newPage(), errors=[]
    page.on('pageerror',e=>errors.push(e.message))
    const base = `http://127.0.0.1:${server.address().port}`
    await page.goto(base)
    await page.getByRole('button',{name:'Skip SRT opening animation'}).click()
    await page.waitForFunction(()=>document.querySelector('.accel-reading')?.textContent==='32%')
    assert.match(await page.locator('.wheel-reading').innerText(),/-32.5°/)
    assert.equal(await page.locator('.wheel-rotor').evaluate(el=>el.style.getPropertyValue('--steering-angle')),'-32.5deg')
    assert.equal(await page.locator('.mode-state').count(),6)
    assert.match(await page.locator('.wheel-pitch').innerText(),/9.0°.*display only/)
    vehicle.switches.remoteMode=true
    await page.getByText('REMOTE MODE ENABLED',{exact:true}).waitFor()
    await page.screenshot({animations:'disabled',path:'/tmp/srt-mode-banner.png'})
    vehicle.switches.remoteMode=false;vehicle.switches.hill=true;vehicle.powertrainReason=10
    await page.getByText('HILL ASSIST UNAVAILABLE',{exact:true}).waitFor()
    assert.match(await page.locator('.hill-unavailable').innerText(),/pedal blocked at idle/)
    vehicle.switches.hill=false;vehicle.powertrainReason=null
    await page.waitForFunction(()=>!document.querySelector('.mode-announcement'))

    assert.equal(await page.locator('a[href*="status"]').count(),0)
    assert.equal(await page.getByRole('button',{name:'CONTROL'}).count(),0)
    assert.equal(await page.locator('a[href*="control"]').count(),0)
    assert.equal(await page.getByText('CAN LOG',{exact:true}).count(),0)
    assert.equal(calls['/api/can-log'] || 0,0);assert.equal(calls['/api/diagnostics'] || 0,0)
    assert.equal(Object.keys(calls).filter(p=>p.startsWith('/tiles/')).length,0)
    for (const [width,height] of [[1280,800],[800,480],[390,844],[320,700]]) {
      await page.setViewportSize({width,height})
      assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'No horizontal overflow')
      if(width>=650)assert.ok(await page.evaluate(()=>document.documentElement.scrollHeight<=innerHeight),'Cockpit fits landscape')
    }
    await page.setViewportSize({width:390,height:844});await page.screenshot({animations:'disabled',path:'/tmp/srt-implemented-home-mobile.png'})
    await page.setViewportSize({width:1280,height:800});await page.screenshot({animations:'disabled',path:'/tmp/srt-implemented-home.png'})
    await page.getByRole('button',{name:'MAP'}).click()
    await page.getByRole('dialog').waitFor()
    await page.waitForFunction(()=>document.querySelectorAll('.leaflet-tile').length>0)
    assert.ok(Object.keys(calls).some(p=>p.startsWith('/tiles/')))
    await page.waitForFunction(()=>[...document.querySelectorAll('.leaflet-tile')].every(t=>t.complete && Number(getComputedStyle(t).opacity)>.99));await page.screenshot({animations:'disabled',path:'/tmp/srt-implemented-map.png'})
    await page.keyboard.press('Escape');assert.equal(await page.getByRole('dialog').count(),0)
    assert.match(await page.evaluate(()=>document.activeElement.textContent),/MAP/)
    assert.equal(calls['/api/steering'] || 0,0)
    assert.equal(calls['/api/drive'] || 0,0)
    for(const route of ['/control/','/control','/controls/','/controls']) {
      const response=await page.goto(base+route)
      assert.equal(response.status(),200)
      await page.getByRole('main',{name:'Remote control'}).waitFor()
      assert.equal(await page.locator('.cockpit,.launch').count(),0)
      assert.equal(await page.getByRole('dialog').count(),0)
    }
    drive.remoteMode=false;drive.ready=false
    await page.goto(base+'/controls');await page.getByText('REMOTE MODE OFF',{exact:true}).waitFor()
    assert.equal(await page.locator('.remote-grid,.joystick-pad,.joystick-layout,.joystick-stop,.horn-hold').count(),0,'Mobile controls are hidden while remote mode is off')
    assert.equal(await page.getByRole('heading',{name:'Turn on the physical REMOTE MODE switch'}).count(),1)
    drive.remoteMode=true;drive.ready=true
    await page.reload()
    await page.getByRole('button',{name:'BUTTONS ↗',exact:true}).click()
    await page.getByRole('heading',{name:'Control',exact:true}).waitFor()
    const left=page.getByRole('button',{name:'Hold left to steer'}),right=page.getByRole('button',{name:'Hold right to steer'})
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    assert.match(await page.locator('.remote-steering').innerText(),/CURRENT ANGLE[\s\S]*-32.5°/)
    await page.setViewportSize({width:390,height:844})
    assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth))
    await page.screenshot({animations:'disabled',path:'/tmp/srt-steering-mobile.png'})
    const horn=page.getByRole('button',{name:'Hold horn to sound'})
    await horn.waitFor();await page.waitForFunction(()=>!document.querySelector('.horn-hold')?.disabled)
    await horn.focus();await page.keyboard.down(' ')
    await page.waitForFunction(()=>document.querySelector('.horn-hold')?.getAttribute('aria-pressed')==='true')
    await page.waitForResponse(r=>r.url().endsWith('/api/horn')&&r.request().method()==='POST')
    assert.equal(hornHttp.state.active,true)
    let hornRelease=page.waitForResponse(r=>r.url().endsWith('/api/horn')&&r.request().method()==='POST'&&new URLSearchParams(r.request().postData()).get('action')==='stop')
    await page.keyboard.up(' ');await hornRelease;await page.waitForFunction(()=>document.querySelector('.horn-hold')?.getAttribute('aria-pressed')==='false')
    assert.equal(hornHttp.state.active,false)
    const press=async button=>{const b=await button.boundingBox();await page.mouse.move(b.x+b.width/2,b.y+b.height/2);await page.mouse.down()}
    const waitCommand=async direction=>{await page.waitForResponse(r=>r.url().endsWith('/api/steering')&&r.request().method()==='POST'&&new URLSearchParams(r.request().postData()).get('direction')===direction)}
    let event=waitCommand('left');await press(left);await event
    assert.equal(steering.direction,1);assert.equal(steering.pwm,60)
    await waitCommand('left') // Holding renews, not a latched click.
    steering.angleAvailable=false;steering.angleDeg=null
    await waitCommand('left')
    assert.equal(steering.direction,1,'Angle loss must not cancel motor-speed control')
    event=waitCommand('stop');await page.mouse.up();await event;assert.equal(steering.direction,0)
    assert.equal(steeringCommands.filter(c=>c.direction==='left').every(c=>c.header==='1'),true)
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    event=waitCommand('right');await press(right);await event;assert.equal(steering.direction,2)
    assert.match(await page.locator('.remote-steering .remote-reading').first().innerText(),/—/)
    event=waitCommand('stop');await page.mouse.move(1,1);await event;await page.mouse.up();assert.equal(steering.direction,0)
    steering.angleAvailable=true;steering.angleDeg=-32.5
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    await left.focus();event=waitCommand('left');await page.keyboard.down('Space');await event
    event=waitCommand('stop');await page.keyboard.up('Space');await event
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    await left.focus();event=waitCommand('left');await page.keyboard.down('Space');await event
    event=waitCommand('stop');await page.keyboard.press('Tab');await event;await page.keyboard.up('Space')
    assert.equal(steering.direction,0,'A keyboard hold still stops when its button loses focus')
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    event=waitCommand('left');await press(left);await event
    event=waitCommand('stop');await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await event;await page.mouse.up();assert.equal(steering.direction,0)
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    event=waitCommand('right');await press(right);await event
    fail.add('/api/steering');await waitCommand('stop');await page.mouse.up()
    const countAtFailure=steeringCommands.length
    fail.delete('/api/steering');await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    assert.equal(steeringCommands.slice(countAtFailure).filter(c=>c.direction!=='stop').length,0,'No automatic resume after reconnect')
    const touch=await context.newCDPSession(page)
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    let box=await left.boundingBox()
    const point={x:box.x+box.width/2,y:box.y+box.height/2,id:1}
    event=waitCommand('left');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point]});await event
    event=waitCommand('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchCancel',touchPoints:[]});await event
    await page.waitForFunction(()=>!document.querySelector('.steering-turn')?.disabled)
    event=waitCommand('left');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point]});await event
    box=await right.boundingBox()
    event=waitCommand('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point,{x:box.x+box.width/2,y:box.y+box.height/2,id:2}]});await event
    await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});assert.equal(steering.direction,0)
    const throttle=page.getByRole('button',{name:'Hold throttle to move'}),reverse=page.getByRole('button',{name:/REVERSE/}),level=page.getByLabel('THROTTLE LEVEL')
    assert.equal(await level.inputValue(),'100','Throttle starts at full-north equivalent');await page.waitForFunction(()=>!document.querySelector('.throttle-hold').disabled)
    await level.focus();await page.keyboard.press('Home');assert.equal(await throttle.isDisabled(),true,'Zero level stays idle');for(let n=0;n<5;n++)await page.keyboard.press('ArrowRight')
    assert.equal(await level.inputValue(),'25')
    await page.waitForFunction(()=>!document.querySelector('.throttle-hold').disabled)
    const waitDrive=action=>page.waitForResponse(r=>r.url().endsWith('/api/drive')&&r.request().method()==='POST'&&new URLSearchParams(r.request().postData()).get('action')===action)
    event=waitDrive('throttle');await press(throttle);await event;assert.equal(drive.throttlePwm,64)
    assert.equal(await reverse.isDisabled(),true);assert.equal(await level.isDisabled(),true)
    event=waitDrive('stop');await page.mouse.up();await event;assert.equal(drive.throttlePwm,0)
    assert.equal(await reverse.isDisabled(),true,'Reverse waits after zero command')
    await page.waitForFunction(()=>!document.querySelector('.reverse-select').disabled)
    event=waitDrive('reverse');await reverse.click();await event;assert.equal(drive.reverse,true)
    await page.waitForFunction(()=>!document.querySelector('.throttle-hold').disabled)
    // Actual two-finger input: independent steering and throttle holds.
    const lb=await left.boundingBox(),tb=await throttle.boundingBox()
    const lp={x:lb.x+lb.width/2,y:lb.y+lb.height/2,id:11},tp={x:tb.x+tb.width/2,y:tb.y+tb.height/2,id:12}
    const rb=await right.boundingBox(),rp={x:rb.x+rb.width/2,y:rb.y+rb.height/2,id:14}
    // Keep throttle held while turning left, releasing steering, then turning right.
    await throttle.focus()
    event=waitDrive('throttle');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[tp]});await event
    for(const [button,steerPoint,direction] of [[left,lp,'left'],[right,rp,'right']]) {
      await page.waitForFunction(()=>!document.querySelector('.steering-turn').disabled)
      await button.focus()
      assert.equal(await throttle.getAttribute('aria-pressed'),'true','Focusing steering preserves the throttle touch hold')
      event=waitCommand(direction);await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[tp,steerPoint]});await event
      await throttle.focus()
      assert.equal(await button.getAttribute('aria-pressed'),'true','Focusing throttle preserves the steering touch hold')
      await Promise.all([waitDrive('throttle'),waitCommand(direction)])
      assert.equal(drive.throttlePwm,64);assert.equal(steering.direction,direction==='left'?1:2)
      event=waitCommand('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[steerPoint]});await event
      assert.equal(steering.direction,0);assert.equal(drive.throttlePwm,64,'Releasing steering preserves throttle')
      await waitDrive('throttle')
    }
    event=waitDrive('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await event
    assert.equal(drive.throttlePwm,0)
    await page.waitForFunction(()=>!document.querySelector('.steering-turn').disabled&&!document.querySelector('.throttle-hold').disabled)
    event=waitCommand('left');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[lp]});await event
    event=waitDrive('throttle');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[lp,tp]});await event
    assert.equal(steering.direction,1);assert.equal(drive.throttlePwm,64)
    event=waitDrive('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[tp]});await event
    assert.equal(drive.throttlePwm,0);assert.equal(steering.direction,1,'Releasing throttle preserves steering hold')
    event=waitCommand('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await event
    await page.waitForFunction(()=>!document.querySelector('.throttle-hold').disabled)
    // A third finger on STOP cancels both independent held controls.
    event=waitCommand('left');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[lp]});await event
    event=waitDrive('throttle');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[lp,tp]});await event
    const sb=await page.locator('.remote-stop').boundingBox()
    const stoppedSteer=waitCommand('stop'),stoppedDrive=waitDrive('stop')
    await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[lp,tp,{id:13,x:sb.x+sb.width/2,y:sb.y+sb.height/2}]})
    await Promise.all([stoppedSteer,stoppedDrive]);assert.equal(steering.direction,0);assert.equal(drive.throttlePwm,0)
    await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]})
    await page.waitForFunction(()=>!document.querySelector('.throttle-hold').disabled)
    event=waitDrive('throttle');await press(throttle);await event
    fail.add('/api/drive');await waitDrive('stop');await page.mouse.up()
    const failureCount=driveCommands.length;fail.delete('/api/drive')
    await page.waitForFunction(()=>!document.querySelector('.throttle-hold').disabled)
    assert.equal(driveCommands.slice(failureCount).filter(c=>c.action==='throttle').length,0)
    event=waitDrive('throttle');await press(throttle);await event
    event=waitDrive('stop');await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await event;await page.mouse.up()
    assert.equal(drive.throttlePwm,0)
    assert.ok(driveCommands.filter(c=>c.action==='throttle').every(c=>c.header==='1'&&c.percent===25))
    for(const [width,height] of [[320,700],[390,844],[800,480]]) {
      await page.setViewportSize({width,height})
      assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth))
      assert.ok(await page.locator('.remote-stop').evaluate(el=>{const r=el.getBoundingClientRect();return r.top>=0&&r.bottom<=innerHeight}),'STOP remains visible')
    }
    await page.setViewportSize({width:390,height:844});await page.screenshot({animations:'disabled',path:'/tmp/srt-control-mobile.png'})
    // Leaving the dedicated page stops both held controls.
    await page.waitForFunction(()=>!document.querySelector('.steering-turn').disabled&&!document.querySelector('.throttle-hold').disabled)
    const exitLeft=await left.boundingBox(),exitThrottle=await throttle.boundingBox()
    const exitPoints=[{id:21,x:exitLeft.x+exitLeft.width/2,y:exitLeft.y+exitLeft.height/2},{id:22,x:exitThrottle.x+exitThrottle.width/2,y:exitThrottle.y+exitThrottle.height/2}]
    event=waitCommand('left');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:exitPoints.slice(0,1)});await event
    event=waitDrive('throttle');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:exitPoints});await event
    const steerBeforeExit=steeringCommands.length,driveBeforeExit=driveCommands.length
    await Promise.all([page.waitForURL(base+'/'),page.getByRole('link',{name:'DASHBOARD'}).evaluate(el=>el.click())])
    // Unload can hide keepalive responses from Playwright; verify server receipt.
    const exitStopsReceived=()=>steeringCommands.slice(steerBeforeExit).some(c=>c.direction==='stop')&&driveCommands.slice(driveBeforeExit).some(c=>c.action==='stop')
    for(let n=0;n<100&&!exitStopsReceived();n++)await new Promise(resolve=>setTimeout(resolve,10))
    assert.ok(exitStopsReceived(),'Both stop commands reach the server when leaving the page')
    await touch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]})
    assert.equal(steering.direction,0);assert.equal(drive.throttlePwm,0)
    await page.goto(base+'/control/')
    await page.getByRole('button',{name:'BUTTONS ↗',exact:true}).click()
    await page.getByRole('heading',{name:'Control',exact:true}).waitFor()
    await Promise.all([page.waitForURL(base+'/'),page.keyboard.press('Escape')])
    await page.waitForFunction(()=>document.querySelector('.wheel-rotor')?.style.getPropertyValue('--steering-angle')==='-32.5deg')
    for(const angle of [-270,-180.1,0,180.1,270]) {
      vehicle.steeringDeg=angle
      await page.waitForFunction(angle=>document.querySelector('.wheel-rotor')?.style.getPropertyValue('--steering-angle')===`${angle}deg`,angle)
      assert.equal(await page.locator('.wheel-figure.unavailable').count(),0)
      assert.equal(await page.locator('.wheel-reading strong').innerText(),`${angle>0?'+':''}${angle.toFixed(1)}°`)
    }
    vehicle.steeringDeg=-32.5
    await page.waitForFunction(()=>document.querySelector('.wheel-rotor')?.style.getPropertyValue('--steering-angle')==='-32.5deg')
    await page.setViewportSize({width:1280,height:800})
    vehicle.steeringDeg=null;vehicle.steeringLink=false;vehicle.acceleratorPct=null
    await page.getByText('UNAVAILABLE',{exact:true}).waitFor()
    assert.equal(await page.locator('.wheel-rotor').evaluate(el=>el.style.getPropertyValue('--steering-angle')),'-32.5deg')
    assert.equal(await page.locator('.mode-state').count(),6)
    assert.match(await page.locator('.wheel-pitch').innerText(),/9.0°.*display only/)
    vehicle.switches.remoteMode=true
    await page.getByText('REMOTE MODE ENABLED',{exact:true}).waitFor()
    await page.screenshot({animations:'disabled',path:'/tmp/srt-mode-banner.png'})
    vehicle.switches.remoteMode=false;vehicle.switches.hill=true;vehicle.powertrainReason=10
    await page.getByText('HILL ASSIST UNAVAILABLE',{exact:true}).waitFor()
    assert.match(await page.locator('.hill-unavailable').innerText(),/pedal blocked at idle/)
    vehicle.switches.hill=false;vehicle.powertrainReason=null
    await page.waitForFunction(()=>!document.querySelector('.mode-announcement'))

    assert.equal(await page.locator('.accel-reading').innerText(),'—%')
    vehicle.acceleratorPct=0
    await page.waitForFunction(()=>document.querySelector('.accel-reading')?.textContent==='0%')
    await page.reload();assert.equal(await page.locator('.launch').count(),0)
    vehicle.acceleratorPct=32;vehicle.steeringDeg=-32.5;vehicle.steeringLink=true
    await page.goto(base+'/status/')
    await page.getByText('CONNECTED',{exact:true}).waitFor()
    await page.locator('.node').nth(0).click();assert.match(await page.locator('.node-detail').innerText(),/Corrected voltage[\s\S]*0.812 V/);assert.match(await page.locator('.node-detail').innerText(),/Steering PWM output/);
    await page.locator('.node').nth(2).click()
    const switchSensors=page.locator('.node-detail .sensor')
    assert.equal(await switchSensors.count(),6)
    const switchLabels=['Cruise','HillHold','Autonomous','Remote mode','Horn','Reverse']
    const switchPins=[4,5,13,14,26,33],switchValues=['ON','OFF','ON','OFF','OFF','ON']
    for(let bit=0;bit<6;bit++) {
      assert.equal(await switchSensors.nth(bit).locator('.sensor-title>span').first().innerText(),switchLabels[bit])
      assert.equal(await switchSensors.nth(bit).locator('strong').innerText(),switchValues[bit])
      assert.match(await switchSensors.nth(bit).locator('small').innerText(),new RegExp(`^GPIO${switchPins[bit]} ·`))
    }
    await page.screenshot({animations:'disabled',path:'/tmp/srt-node3-six-switches.png'})
    await page.locator('.node').nth(1).click();assert.match(await page.locator('.node-detail').innerText(),/NO FEEDBACK/)
    const savedMotor={...vehicle.nodes[1]}
    Object.assign(vehicle.nodes[1],{alive:true,source:'heartbeat',status:0,error:5})
    Object.assign(vehicle,{throttleOut:0,reverseActive:false,powertrainReason:5})
    const motorSignal={id:0x201,seen:true,fresh:true,ageMs:10,invalid:0,timeoutMs:250}
    diagnostics.signals.push(motorSignal)
    await page.waitForFunction(()=>document.querySelector('.node-detail')?.textContent.includes('release pedal to resume'))
    assert.match(await page.locator('.node').nth(1).innerText(),/FAULT/)
    vehicle.powertrainReason=0;vehicle.nodes[1].status=1;vehicle.nodes[1].error=0
    await page.waitForFunction(()=>[...document.querySelectorAll('.node-detail .sensor')].some(el=>el.textContent.includes('Motor stop reason')&&el.textContent.includes('ready')))
    motorSignal.fresh=false
    await page.waitForFunction(()=>[...document.querySelectorAll('.node-detail .sensor')].some(el=>el.textContent.includes('Motor stop reason')&&el.textContent.includes('UNAVAILABLE')))
    Object.assign(vehicle.nodes[1],savedMotor)
    vehicle.powertrainReason=null;vehicle.throttleOut=null;vehicle.reverseActive=null
    assert.equal(await page.locator('.node').count(),4)
    await page.locator('.node').nth(3).click();assert.match(await page.locator('.node-detail').innerText(),/GPS receiver/)
    assert.match(await page.locator('.node-detail').innerText(),/136.7 KB/)
    const filter=page.getByLabel('CAN ID'), rows=page.locator('.message')
    for(const id of ['301',' 0X00000301 ']) { await filter.fill(id); assert.equal(await rows.count(),1); assert.match(await rows.innerText(),/ADC 2000\/4095/) }
    await rows.locator('summary').click();assert.match(await rows.locator('.raw').innerText(),/D0 07/)
    for(const id of ['0x','xyz','301junk','20000000','-1']) { await filter.fill(id);assert.equal(await filter.getAttribute('aria-invalid'),'true');assert.match(await page.locator('.empty').innerText(),/Invalid ID/) }
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByLabel('NODE',{exact:true}).selectOption('3');assert.equal(await rows.count(),1)
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByLabel('SEARCH',{exact:true}).fill('D0 07');assert.equal(await rows.count(),1)
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByLabel('Issues only').check();assert.equal(await rows.count(),1);assert.match(await rows.innerText(),/pedal missing/)
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByRole('button',{name:'PAUSE'}).click()
    frames=[...frames,{seq:4,ms:2000,id:0x300,dlc:1,data:[0]}]
    await page.waitForResponse(r=>r.url().endsWith('/api/can-log'));assert.equal(await rows.count(),3)
    await page.getByRole('button',{name:'RESUME'}).click();await page.waitForFunction(()=>document.querySelectorAll('.message').length===4)
    await page.getByRole('button',{name:'COPY',exact:true}).click();await page.getByText('Copied',{exact:true}).waitFor()
    const copied=JSON.parse(await page.evaluate(()=>navigator.clipboard.readText()));assert.equal(copied.frames.length,4)
    const downloadPromise=page.waitForEvent('download');await page.getByRole('button',{name:'EXPORT'}).click()
    const download=await downloadPromise;const exported=JSON.parse(fs.readFileSync(await download.path(),'utf8'));assert.equal(exported.frames.length,4)
    gps.fix=false;gps.receiving=false;gps.sats=0
    await page.locator('.node').nth(3).click()
    await page.waitForFunction(()=>document.querySelectorAll('.node-detail .sensor.fault').length>0)
    await page.waitForFunction(()=>document.querySelector('.node-detail')?.textContent.includes('No recent UART data'))
    fail.add('/api/vehicle')
    await page.getByText('DISCONNECTED',{exact:true}).waitFor()
    assert.match(await page.locator('.node').nth(0).innerText(),/UNKNOWN/)
    fail.clear();gps.fix=true;gps.receiving=true;gps.sats=8
    await page.getByText('CONNECTED',{exact:true}).waitFor()
    await page.getByText(/Recovered · vehicle API/).waitFor()
    for(const [width,height] of [[390,844],[320,700],[800,480]]) {
      await page.setViewportSize({width,height})
      assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Status fits mobile')
    }
    await page.setViewportSize({width:390,height:844});await page.screenshot({animations:'disabled',path:'/tmp/srt-implemented-status-mobile.png',fullPage:true})
    await page.goto(base+'/status');await page.getByText('CONNECTED',{exact:true}).waitFor()
    const reduced=await browser.newPage({reducedMotion:'reduce'});await reduced.goto(base);assert.equal(await reduced.locator('.launch').count(),0)
    assert.deepEqual(errors,[])
    console.log('Packed drive passed: adjustable full-north default level, reverse lock, throttle-first left/right multi-touch across focus changes, independent release, shared STOP and recovery')
    console.log('Packed steering passed: pointer/keyboard release, drag-away, blur, reconnect, touch cancel and conflicting multi-touch')
    console.log('Packed motor diagnostics passed: named stop reason, idle recovery, heartbeat health and stale output telemetry')
    console.log('Packed UI passed: direct /control and /control/ routes, refresh, no homepage control button, stop on navigation, layouts, lazy map, wireless steering angle/freeze, unknown feedback, filters, pause, copy/export, API failures, recovery, reduced motion')
  } finally {await browser.close();server.close()}
})().catch(e=>{console.error(e);server.close();process.exitCode=1})
