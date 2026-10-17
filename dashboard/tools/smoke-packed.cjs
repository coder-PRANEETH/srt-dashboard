/* Browser smoke test of the compressed Node 5 site. Set PLAYWRIGHT_MODULE
 * to an existing Playwright installation and BROWSER_PATH if needed. */
const assert = require('node:assert/strict')
const fs = require('node:fs')
const http = require('node:http')
const path = require('node:path')
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright')
const root = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'node5_dash', 'production', 'data'))
assert.ok(fs.existsSync(path.join(root, 'index.html.gz')))
const vehicle = {
  canReady: true, frames: 42, steeringDeg: 12.3, steeringLink: true,
  horn: false, headlight: null, immoState: null, motor1: false, motor2: false,
  reverseActive: false, throttleOut: 0, cruiseActive: false, ecoActive: false,
  hillActive: false, acceleratorAdc: 2000, acceleratorPct: 48,
  throttleBlocked: false, brake: null, ignition: true, ignitionUptime: 120,
  hillRequest: true, pitchDeg: 5, pitchValid: true, holdPct: 0, summonActive: null,
  switches: { cruise: false, eco: false, reverse: false, hill: false,
    horn: false, autonomous: false, headlight: false },
  nodes: [1, 2, 3, 4, 5].map(id => ({ id, alive: id !== 3, status: 1, error: 0 })),
}
const frames = [{ seq: 1, ms: 1000, id: 0x301, dlc: 8,
  data: [7, 208, 48, 0, 0, 0, 0, 0] }]
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
    assert.match(await page.locator('.node-health').innerText(), /NODE 3 OFFLINE/)
    assert.match(await page.locator('.vehicle-panel').innerText(), /48%/)
    assert.match(await page.locator('.vehicle-panel').innerText(), /Hill Request\s+ON/)
    assert.match(await page.locator('.vehicle-panel').innerText(), /Pitch\s+5°/)
    assert.ok(await page.evaluate(() => document.documentElement.scrollHeight >= document.querySelector('.vehicle-panel').getBoundingClientRect().bottom), 'Telemetry panel must be scrollable')
    if (process.env.SMOKE_SCREENSHOT) await page.screenshot({ path: process.env.SMOKE_SCREENSHOT, fullPage: true })
    await page.getByRole('button', { name: 'CAN LOG' }).click()
    await page.getByText('Pedal ADC 2000').waitFor()
    assert.deepEqual(errors, [])
    console.log('Packed dashboard smoke test passed')
  } finally { await browser.close(); server.close() }
})().catch(error => { console.error(error); server.close(); process.exitCode = 1 })
