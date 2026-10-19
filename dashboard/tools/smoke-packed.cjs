/* Browser smoke test of the compressed Node 5 site. Set PLAYWRIGHT_MODULE
 * to an existing Playwright installation and BROWSER_PATH if needed. */
const assert = require('node:assert/strict')
const fs = require('node:fs')
const http = require('node:http')
const path = require('node:path')
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright')
const root = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'data'))
assert.ok(fs.existsSync(path.join(root, 'index.html.gz')))
const vehicle = {
  canReady: true, frames: 42,
  acceleratorAdc: 2000, acceleratorPct: 48, acceleratorVoltageEstimate: 1.612,
  ignition: true, ignitionUptime: 120,
  hillRequest: true, pitchDeg: 5, pitchValid: true, holdPct: 0,
  switches: { cruise: true, eco: false, reverse: true, hill: false,
    horn: false, headlight: true, autonomous: false },
  nodes: [
    { id: 1, alive: true, status: null, source: 'data' },
    { id: 2, alive: null, status: null, source: 'receive-only' },
    { id: 3, alive: true, status: null, source: 'data' },
    { id: 4, alive: false, status: 0, source: 'heartbeat' },
    { id: 5, alive: true, status: 1, source: 'local' },
  ],
}
const frames = [
  { seq: 1, ms: 1000, id: 0x301, dlc: 2, data: [0xD0, 0x07] },
  { seq: 2, ms: 1000, id: 0x300, dlc: 1, data: [0x25] },
]
const mime = { '.html': 'text/html', '.js': 'application/javascript',
  '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml' }
const server = http.createServer((req, res) => {
  const pathname = new URL(req.url, 'http://localhost').pathname
  const api = {
    '/api/vehicle': vehicle,
    '/api/gps': { fix: true, lat: 10.728, lon: 79.0195, sats: 8, speed: 20, ageMs: 200 },
    '/api/can-log': { ready: true, total: 42, frames },
  }
  if (Object.hasOwn(api, pathname)) {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' })
    res.end(JSON.stringify(api[pathname])); return
  }
  const relative = pathname === '/' ? 'index.html' : pathname.slice(1)
  const target = path.resolve(root, relative)
  if (!target.startsWith(root + path.sep)) { res.writeHead(403); res.end(); return }
  const gz = fs.existsSync(target + '.gz')
  const file = gz ? target + '.gz' : target
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404); res.end(); return }
  const headers = { 'Content-Type': mime[path.extname(target)] || 'application/octet-stream' }
  if (gz) headers['Content-Encoding'] = 'gzip'
  res.writeHead(200, headers); fs.createReadStream(file).pipe(res)
})
;(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const browser = await chromium.launch({ headless: true,
    ...(process.env.BROWSER_PATH ? { executablePath: process.env.BROWSER_PATH } : {}),
    args: ['--no-sandbox'] })
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 900 } })
    const errors = []
    page.on('pageerror', e => errors.push(e.message))
    await page.goto(`http://127.0.0.1:${server.address().port}/`)
    await page.getByText('42 frames received').waitFor()
    assert.match(await page.locator('.node-health').innerText(), /NODE 3 ONLINE/)
    assert.match(await page.locator('.vehicle-panel').innerText(), /48%/)
    assert.match(await page.locator('.node-health').innerText(), /NODE 2 NO TELEMETRY/)
    assert.match(await page.locator('.vehicle-panel').innerText(), /GPIO34 Voltage Estimate\s+1.612 V/i)
    assert.match(await page.locator('.vehicle-panel').innerText(), /Headlight\s+ON/i)
    assert.match(await page.locator('.vehicle-panel').innerText(), /Autonomous\s+OFF/i)
    await page.getByText('ACCELERATOR INPUT', { exact: true }).waitFor()
    assert.match(await page.locator('.vehicle-panel').innerText(), /Hill Request\s+ON/)
    assert.match(await page.locator('.vehicle-panel').innerText(), /Pitch\s+5°/)
    assert.ok(await page.evaluate(() => document.documentElement.scrollHeight >= document.querySelector('.vehicle-panel').getBoundingClientRect().bottom), 'Telemetry panel must be scrollable')
    if (process.env.SMOKE_SCREENSHOT) await page.screenshot({ path: process.env.SMOKE_SCREENSHOT, fullPage: true })
    await page.getByRole('button', { name: 'CAN LOG' }).click()
    await page.getByText(/Pedal ADC 2000\/4095/).waitFor()
    await page.getByText(/headlight ON · autonomous OFF/).waitFor()
    const filter = page.getByRole('combobox', { name: 'Filter by CAN ID' })
    const rows = page.locator('.can-log-table tbody tr')
    await filter.fill('0x301')
    assert.equal(await rows.count(), 1)
    assert.match(await rows.innerText(), /Pedal ADC 2000/)
    assert.equal(await rows.locator('.can-bytes').innerText(), 'D0 07')
    // The next poll must retain the filter.
    await page.waitForResponse(response => response.url().endsWith('/api/can-log'))
    assert.equal(await rows.count(), 1)
    await filter.fill(' 300 ')
    assert.equal(await rows.count(), 1)
    assert.match(await rows.innerText(), /headlight ON · autonomous OFF/)
    assert.equal(await rows.locator('.can-bytes').innerText(), '25')
    await filter.fill('0X00000301')
    assert.match(await rows.innerText(), /Pedal ADC 2000/)
    await filter.fill('1FFFFFFF')
    assert.match(await rows.innerText(), /No matching CAN frames/)
    for (const invalid of ['0x', 'xyz', '301junk', '20000000', '-1']) {
      await filter.fill(invalid)
      assert.equal(await filter.getAttribute('aria-invalid'), 'true')
      assert.match(await rows.innerText(), /Invalid CAN ID/)
    }
    await page.getByRole('button', { name: 'Clear filter' }).click()
    assert.equal(await filter.inputValue(), '')
    assert.equal(await rows.count(), 2)
    await page.setViewportSize({ width: 390, height: 844 })
    await filter.fill('301')
    assert.match(await rows.innerText(), /Pedal ADC 2000/)
    for (const control of [page.getByRole('dialog'), filter, page.getByRole('button', { name: 'Clear filter' }), page.getByRole('button', { name: 'Close CAN log' })]) {
      const box = await control.boundingBox()
      assert.ok(box && box.x >= 0 && box.x + box.width <= 390, 'CAN log controls must fit the mobile viewport')
    }
    if (process.env.SMOKE_CAN_SCREENSHOT) await page.screenshot({ path: process.env.SMOKE_CAN_SCREENSHOT })
    await page.getByRole('button', { name: 'Close CAN log' }).click()
    vehicle.acceleratorAdc = vehicle.acceleratorPct = vehicle.acceleratorVoltageEstimate = null
    vehicle.switches = Object.fromEntries(Object.keys(vehicle.switches).map(key => [key, null]))
    vehicle.nodes[0].alive = vehicle.nodes[2].alive = false
    await page.getByText('NODE 1 OFFLINE', { exact: true }).waitFor()
    await page.getByText('NODE 3 OFFLINE', { exact: true }).waitFor()
    assert.match(await page.locator('.vehicle-panel').innerText(), /Accelerator ADC\s+—/i)
    assert.deepEqual(errors, [])
    console.log('Packed dashboard smoke test passed')
  } finally { await browser.close(); server.close() }
})().catch(error => { console.error(error); server.close(); process.exitCode = 1 })
