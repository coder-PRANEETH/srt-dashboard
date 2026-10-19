/* Exercises the actual compressed ESP32 bundle. No physical board required. */
const assert = require('node:assert/strict')
const fs = require('node:fs')
const http = require('node:http')
const path = require('node:path')
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright')
const root = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'data'))
const vehicle = {
  canReady: true, frames: 42, acceleratorAdc: 2000, acceleratorPct: 48, acceleratorVoltageEstimate: 1.612,
  ignition: true, ignitionUptime: 120, hillRequest: true, pitchDeg: 5, pitchValid: true, holdPct: 0,
  switches: { cruise: true, eco: false, reverse: true, hill: false, horn: false, headlight: true, autonomous: false },
  nodes: [1,2,3,4,5].map(id => ({ id, alive: id === 2 ? null : true, status: id === 2 ? null : 1, error: 0, ageMs: 10 })),
}
const gps = { fix: true, lat: 10.728, lon: 79.0195, sats: 8, speed: 20, ageMs: 20 }
const diagnostics = { uptimeMs: 5000, freeHeap: 140000, minFreeHeap: 130000, largestFreeBlock: 110000, fsReady: true, fsTotal: 1966080, fsUsed: 1500000, clients: 1, canReady: true, rxTotal: 42, txTotal: 5, txFailed: 0, lastRxAgeMs: 20, malformedFrames: 0, unknownFrames: 0, signals: [0x301,0x300,0x400,0x401,0x500].map(id => ({ id, seen: true, fresh: true, ageMs: 20, invalid: 0, timeoutMs: id === 0x301 ? 501 : 3000 })) }
let frames = [
  { seq: 1, ms: 1000, id: 0x301, dlc: 2, data: [0xD0,0x07] },
  { seq: 2, ms: 1000, id: 0x300, dlc: 1, data: [0x25] },
  { seq: 3, ms: 1000, id: 0x500, dlc: 8, data: [1,90,0,0,0,0,0,0] },
]
const calls = {}, fail = new Set()
const mime = { '.html':'text/html', '.js':'application/javascript', '.css':'text/css', '.png':'image/png', '.svg':'image/svg+xml' }
const server = http.createServer((req,res) => {
  const pathname = new URL(req.url,'http://localhost').pathname
  calls[pathname] = (calls[pathname] || 0)+1
  const api = { '/api/vehicle':vehicle, '/api/gps':gps, '/api/diagnostics':diagnostics, '/api/can-log':{ ready:true, total:42, uptimeMs:5000, frames } }
  if (Object.hasOwn(api,pathname)) {
    res.writeHead(fail.has(pathname) ? 503 : 200, { 'Content-Type':'application/json', 'Cache-Control':'no-store' }); res.end(JSON.stringify(api[pathname])); return
  }
  const relative = ['/', '/status', '/status/'].includes(pathname) ? 'index.html' : pathname.slice(1)
  const target = path.resolve(root,relative), gz = fs.existsSync(target+'.gz'), file = gz ? target+'.gz' : target
  if (!target.startsWith(root+path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404);res.end();return }
  res.writeHead(200,{ 'Content-Type':mime[path.extname(target)] || 'application/octet-stream', ...(gz ? {'Content-Encoding':'gzip'} : {}) });fs.createReadStream(file).pipe(res)
})
;(async () => {
  await new Promise(resolve => server.listen(0,'127.0.0.1',resolve))
  const browser = await chromium.launch({headless:true, executablePath:process.env.BROWSER_PATH, args:['--no-sandbox']})
  try {
    const context = await browser.newContext({viewport:{width:1280,height:800}, permissions:['clipboard-read','clipboard-write']})
    const page = await context.newPage(), errors=[]
    page.on('pageerror',e=>errors.push(e.message))
    const base = `http://127.0.0.1:${server.address().port}`
    await page.goto(base)
    await page.getByRole('button',{name:'Skip SRT opening animation'}).click()
    await page.waitForFunction(()=>document.querySelector('.accel-reading')?.textContent==='48%')
    assert.match(await page.locator('.wheel-reading').innerText(),/\+5.0°/)
    assert.equal(await page.locator('a[href*="status"]').count(),0)
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
    await page.waitForFunction(()=>[...document.querySelectorAll('.leaflet-tile')].every(t=>t.complete));await page.screenshot({animations:'disabled',path:'/tmp/srt-implemented-map.png'})
    await page.keyboard.press('Escape');assert.equal(await page.getByRole('dialog').count(),0)
    assert.match(await page.evaluate(()=>document.activeElement.textContent),/MAP/)
    vehicle.pitchDeg=null;vehicle.pitchValid=false;vehicle.acceleratorPct=null
    await page.getByText('UNAVAILABLE',{exact:true}).waitFor()
    assert.equal(await page.locator('.wheel-rotor').evaluate(el=>el.style.getPropertyValue('--pitch')),'5deg')
    assert.equal(await page.locator('.accel-reading').innerText(),'—%')
    vehicle.acceleratorPct=0
    await page.waitForFunction(()=>document.querySelector('.accel-reading')?.textContent==='0%')
    await page.reload();assert.equal(await page.locator('.launch').count(),0)
    vehicle.acceleratorPct=48;vehicle.pitchDeg=5;vehicle.pitchValid=true
    await page.goto(base+'/status/')
    await page.getByText('CONNECTED',{exact:true}).waitFor()
    await page.locator('.node').nth(1).click();assert.match(await page.locator('.node-detail').innerText(),/NO FEEDBACK/)
    await page.locator('.node').nth(3).click();assert.match(await page.locator('.node-detail').innerText(),/MPU6050 pitch/)
    await page.locator('.node').nth(4).click();assert.match(await page.locator('.node-detail').innerText(),/136.7 KB/)
    const filter=page.getByLabel('CAN ID'), rows=page.locator('.message')
    for(const id of ['301',' 0X00000301 ']) { await filter.fill(id); assert.equal(await rows.count(),1); assert.match(await rows.innerText(),/ADC 2000\/4095/) }
    await rows.locator('summary').click();assert.match(await rows.locator('.raw').innerText(),/D0 07/)
    for(const id of ['0x','xyz','301junk','20000000','-1']) { await filter.fill(id);assert.equal(await filter.getAttribute('aria-invalid'),'true');assert.match(await page.locator('.empty').innerText(),/Invalid ID/) }
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByLabel('NODE',{exact:true}).selectOption('3');assert.equal(await rows.count(),1)
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByLabel('SEARCH',{exact:true}).fill('D0 07');assert.equal(await rows.count(),1)
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByLabel('Issues only').check();assert.equal(await rows.count(),1);assert.match(await rows.innerText(),/Pitch unavailable/)
    await page.getByRole('button',{name:'RESET',exact:true}).click()
    await page.getByRole('button',{name:'PAUSE'}).click()
    frames=[...frames,{seq:4,ms:2000,id:0x401,dlc:8,data:[1,0,120,0,0,0,0,0]}]
    await page.waitForResponse(r=>r.url().endsWith('/api/can-log'));assert.equal(await rows.count(),3)
    await page.getByRole('button',{name:'RESUME'}).click();await page.waitForFunction(()=>document.querySelectorAll('.message').length===4)
    await page.getByRole('button',{name:'COPY',exact:true}).click();await page.getByText('Copied',{exact:true}).waitFor()
    const copied=JSON.parse(await page.evaluate(()=>navigator.clipboard.readText()));assert.equal(copied.frames.length,4)
    const downloadPromise=page.waitForEvent('download');await page.getByRole('button',{name:'EXPORT'}).click()
    const download=await downloadPromise;const exported=JSON.parse(fs.readFileSync(await download.path(),'utf8'));assert.equal(exported.frames.length,4)
    vehicle.pitchValid=false;vehicle.pitchDeg=null;vehicle.nodes[3].error=2;vehicle.nodes[3].status=0
    await page.locator('.node').nth(3).click()
    await page.waitForFunction(()=>document.querySelectorAll('.node-detail .sensor.fault').length>0)
    assert.match(await page.locator('.node-detail').innerText(),/error 2/)
    fail.add('/api/vehicle')
    await page.getByText('DISCONNECTED',{exact:true}).waitFor()
    assert.match(await page.locator('.node').nth(0).innerText(),/UNKNOWN/)
    fail.clear();vehicle.pitchValid=true;vehicle.pitchDeg=5;vehicle.nodes[3].error=0;vehicle.nodes[3].status=1
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
    console.log('Packed UI passed: routes, layouts, lazy map, IMU freeze, unknown feedback, filters, pause, copy/export, API failures, recovery, reduced motion')
  } finally {await browser.close();server.close()}
})().catch(e=>{console.error(e);server.close();process.exitCode=1})
