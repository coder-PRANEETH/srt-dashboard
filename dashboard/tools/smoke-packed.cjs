/*
 * Browser smoke test for the packed LittleFS site, using API fixtures.
 * Run after `npm run build:esp`: node tools/smoke-packed.cjs [data-folder]
 * Requires an optional Playwright installation and Chromium:
 *   npm install --no-save --package-lock=false playwright
 *   npx playwright install chromium
 * Set PLAYWRIGHT_MODULE for an existing Playwright package and BROWSER_PATH
 * for an existing Chromium browser. Set SMOKE_SCREENSHOT to save an image.
 * This checks the packaged website, not physical ESP32/CAN/GPS hardware.
 */
const assert = require('node:assert/strict')
const fs = require('node:fs')
const http = require('node:http')
const path = require('node:path')
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright')

const dataRoot = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'esp32_dashboard_host', 'data'))
assert.ok(fs.existsSync(path.join(dataRoot, 'index.html.gz')), 'Packed site missing: run npm run build:esp first')
const count = { '/api/data': 0, '/api/gps': 0, '/api/can-log': 0 }
let gps = { lat: 0, lon: 0, sats: 0, fix: false }
let dialHang = false
const mime = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml' }
const canFrames = [
  { seq: 1, ms: 1000, id: 0x100, dlc: 8, data: [0, 0, 0, 0, 0, 0, 0, 0] },
  { seq: 2, ms: 1001, id: 0x101, dlc: 8, data: [89, 1, 206, 4, 9, 1, 120, 0] },
  { seq: 3, ms: 1002, id: 0x102, dlc: 8, data: [42, 0, 0, 0, 60, 0, 0, 0] },
]

const server = http.createServer((req, res) => {
  const pathname = new URL(req.url, 'http://localhost').pathname
  if (Object.hasOwn(count, pathname)) {
    count[pathname] += 1
    if (pathname === '/api/data' && dialHang) return
    const json = pathname === '/api/data' ? { value: 75.4 } :
      pathname === '/api/gps' ? gps : { ready: true, total: 3, frames: canFrames }
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' })
    res.end(JSON.stringify(json))
    return
  }
  const logical = pathname.endsWith('/') ? pathname + 'index.html' : pathname
  const target = path.join(dataRoot, logical)
  const gzip = fs.existsSync(target + '.gz')
  const filename = gzip ? target + '.gz' : target
  if (!fs.existsSync(filename) || !fs.statSync(filename).isFile()) {
    res.writeHead(404, { 'Content-Type': 'text/plain' })
    res.end('no tile')
    return
  }
  const headers = {
    'Content-Type': mime[path.extname(logical)] || 'text/plain',
    'Cache-Control': pathname.startsWith('/tiles/') ? 'max-age=31536000' : 'no-cache',
  }
  if (gzip) headers['Content-Encoding'] = 'gzip'
  res.writeHead(200, headers)
  fs.createReadStream(filename).pipe(res)
})

;(async () => {
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
  const origin = `http://127.0.0.1:${server.address().port}`
  const browser = await chromium.launch({ ...(process.env.BROWSER_PATH ? { executablePath: process.env.BROWSER_PATH } : {}), headless: true, args: ['--no-sandbox'] })
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } })
    const errors = []
    const external = []
    const assetResponses = []
    page.on('pageerror', (error) => errors.push(error.message))
    page.on('response', (response) => {
      if (/\.(?:js|css)$/.test(new URL(response.url()).pathname)) {
        assetResponses.push({ url: response.url(), status: response.status(), encoding: response.headers()['content-encoding'] })
      }
    })
    await page.route('**/*', (route) => {
      const url = route.request().url()
      if (/^https?:/.test(url) && !url.startsWith(origin + '/')) {
        external.push(url)
        return route.abort()
      }
      return route.continue()
    })
    await page.goto(origin)
    await page.locator('.gauge').nth(1).locator('.gauge-value').waitFor()
    await page.waitForFunction(() => document.querySelectorAll('.gauge-value')[1].textContent === '75')
    assert.equal(await page.locator('.gauge-value').first().textContent(), '—')
    assert.match(await page.locator('.camera .gps-pill').textContent(), /NO FIX/)
    assert.equal(await page.evaluate(() => getComputedStyle(document.body).backgroundColor), 'rgb(5, 8, 7)')
    assert.equal(assetResponses.length, 2)
    assert.ok(assetResponses.every((response) => response.status === 200 && response.encoding === 'gzip'))
    const before = { ...count }
    await page.waitForTimeout(2000)
    const pollCounts = { dial: count['/api/data'] - before['/api/data'], gps: count['/api/gps'] - before['/api/gps'] }
    assert.ok(pollCounts.dial >= 15 && pollCounts.dial <= 22, JSON.stringify(pollCounts))
    assert.ok(pollCounts.gps >= 3 && pollCounts.gps <= 5, JSON.stringify(pollCounts))

    gps = { lat: 10.728, lon: 79.0195, sats: 9, speed: 34.5, course: 123, hdop: 1.2, ageMs: 0, fix: true }
    await page.waitForFunction(() => document.querySelector('.camera-position').textContent.includes('10.728000'))
    await page.waitForFunction(() => document.querySelector('.camera-stats').textContent.includes('123°'))
    await page.waitForFunction(() => document.querySelectorAll('.gauge-value')[0].textContent === '34' || document.querySelectorAll('.gauge-value')[0].textContent === '35')
    gps = { ...gps, lat: 10.72805, course: 250 }
    await page.waitForFunction(() => document.querySelector('.camera-position').textContent.includes('10.728050'))
    await page.waitForFunction(() => document.querySelector('.camera-stats').textContent.includes('250°'))
    gps = { ...gps, course: 270 }
    await page.waitForFunction(() => document.querySelector('.camera-stats').textContent.includes('270°'))
    gps = { ...gps, course: undefined }
    await page.waitForFunction(() => document.querySelectorAll('.camera-stats strong')[2].textContent === '0°')
    assert.ok(await page.locator('.leaflet-tile').count() > 0)
    assert.ok(await page.evaluate(() => [...document.querySelectorAll('.leaflet-tile')].every((tile) => tile.complete && tile.naturalWidth > 0)))

    await page.getByRole('button', { name: 'CAN LOG', exact: true }).click()
    await page.getByRole('dialog').waitFor()
    await page.waitForFunction(() => document.querySelector('.can-log-status').textContent.includes('CAN MODULE READY'))
    assert.equal(await page.locator('.can-log-table tbody tr').count(), 3)
    assert.match(await page.locator('.can-log-table tbody tr').first().textContent(), /Heartbeat 42/)
    assert.match(await page.locator('.can-log-table tbody tr').nth(1).textContent(), /HDOP 1.20/)
    await page.keyboard.press('Escape')
    await page.getByRole('dialog').waitFor({ state: 'detached' })
    await page.getByRole('button', { name: 'VIEW ROUTE', exact: true }).click()
    await page.locator('.mapcard.expanded').waitFor()
    await page.getByRole('button', { name: 'Exit fullscreen map' }).click()
    await page.locator('.mapcard.expanded').waitFor({ state: 'detached' })

    gps = { lat: 0, lon: 0, sats: 0, fix: false }
    const lostAt = Date.now()
    await page.waitForFunction(() => document.querySelector('.camera .gps-pill').textContent.includes('STALE'), null, { timeout: 1500 })
    const noFixStatusDelayMs = Date.now() - lostAt
    assert.match(await page.locator('.camera-position').textContent(), /10.728050, 79.019500/)
    assert.equal(await page.locator('.gauge-value').first().textContent(), '—')
    dialHang = true
    await page.waitForFunction(() => document.querySelectorAll('.gauge-value')[1].textContent === '—', null, { timeout: 5000 })
    dialHang = false
    await page.waitForFunction(() => document.querySelectorAll('.gauge-value')[1].textContent === '75', null, { timeout: 6000 })

    assert.deepEqual(errors, [])
    assert.deepEqual(external, [])
    if (process.env.SMOKE_SCREENSHOT) await page.screenshot({ path: process.env.SMOKE_SCREENSHOT })
    console.log(JSON.stringify({ result: 'PASS', packedGzipAssets: assetResponses, twoSecondPollCounts: pollCounts, noFixStatusDelayMs, checks: ['offline initial render', 'CSS decompression', 'local map tile/fallback images', 'GPS speed/position/course', 'course after movement and while stationary', 'bearing fallback', 'CAN frame display/decode/order', 'CAN log Escape close', 'route expansion/collapse', 'immediate explicit GPS fix loss preserves position', 'dial timeout/recovery', 'no external runtime requests', 'no JavaScript errors'] }, null, 2))
  } finally {
    await browser.close()
    server.closeAllConnections()
    await new Promise((resolve) => server.close(resolve))
  }
})().catch((error) => { console.error(error); process.exitCode = 1; server.closeAllConnections(); server.close() })
