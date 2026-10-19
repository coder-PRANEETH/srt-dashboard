# Routes

No client router is installed. `/` renders dashboard/src/App.jsx via dashboard/src/main.jsx. It has a top rail, two gauges, GPS summary, offline route map, lower detailed telemetry, and a CAN-log modal. Firmware currently maps `/status` to raw sendVehicle JSON; it is NOT a separate rendered page. The requested redesign must serve a diagnostics UI at `/status` and remove the CAN entry point and lower telemetry from `/`, without any homepage link to `/status`. APIs remain `/api/vehicle`, `/api/gps`, `/api/data`, `/api/can-log`. Vite uses a relative base and firmware falls back to index.html; account for direct loading and trailing slashes when implementing routes.

## `dashboard/vite.config.js`

```js
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import process from 'node:process'

/*
 * ESP32 #1 hosts this build from LittleFS and exposes /api/data
 * (potentiometer) and /api/gps (relayed from ESP32 #2, which owns the
 * Ready-to-Sky module).
 *
 * Set DASH_ESP_HOST when developing against the real board:
 *   DASH_ESP_HOST=http://192.168.4.1 npm run dev
 */
const ESP_HOST = process.env.DASH_ESP_HOST || 'http://192.168.4.1'

/*
 * Vite marks its generated <script> and <link rel=stylesheet> tags with
 * crossorigin. That attribute makes the browser do a CORS check, and the
 * ESP32 only sends Access-Control-Allow-Origin on /api/* - never on
 * static files. The result: the CSS downloads fine but the browser
 * discards it, and the dashboard renders completely unstyled.
 *
 * Everything here is same-origin (the board serves the page and its
 * assets), so the attribute buys nothing. Strip it.
 */
const stripCrossorigin = {
  name: 'strip-crossorigin',
  transformIndexHtml(html) {
    return html.replace(/\s+crossorigin(?==|\s|>)(="[^"]*")?/g, '')
  },
}

export default defineConfig({
  plugins: [react(), stripCrossorigin],

  /*
   * Relative asset URLs. The board serves everything from the filesystem
   * root, and a relative base keeps the page working if it is ever opened
   * from a subpath or straight off disk.
   */
  base: './',

  build: {
    /*
     * Flash is the scarce resource here, so squeeze the bundle: no
     * sourcemaps, and inline nothing as base64 (a data: URI would be
     * stored uncompressed inside the JS, while a real file next to it
     * gets gzipped by tools/pack-fs.py).
     */
    sourcemap: false,
    assetsInlineLimit: 0,
    chunkSizeWarningLimit: 600,

    /*
     * One JS file and one CSS file. Each extra chunk costs a whole 4 KB
     * LittleFS block and another request over a slow SoftAP link.
     *
     * The short content hash in the name is what makes a rebuilt site
     * actually reach the browser. With fixed names (app.js/index.css) a
     * cached copy shadows the new one forever and the dashboard renders
     * with stale or missing CSS. A new hash means a new URL, which no
     * cache can have seen before.
     */
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
        entryFileNames: 'assets/app-[hash:8].js',
        chunkFileNames: 'assets/app-[hash:8].js',
        assetFileNames: 'assets/[name]-[hash:8][extname]',
      },
    },
  },

  server: {
    proxy: {
      '/api': { target: ESP_HOST, changeOrigin: true },
    },
  },
})

```

## Firmware HTTP registration
```cpp
  server.on("/api/data", sendSpeed); server.on("/api/gps", sendGps);
  server.on("/api/vehicle", sendVehicle); server.on("/api/can-log", sendCanLog);
  server.on("/status", [](){ sendVehicle(); });
  server.onNotFound([](){
    if (server.uri().startsWith("/api/")) { server.send(404,"application/json","{\"error\":\"unknown API\"}"); return; }
    if (sendFile(server.uri())) return;
    if (server.uri().startsWith("/tiles/")) { server.send(404,"text/plain","no tile"); return; }
    if (!sendFile("/index.html")) server.send(404,"text/plain","website missing: upload LittleFS data");
  });
  server.begin(); Serial.print("Dashboard: http://"); Serial.println(WiFi.softAPIP());
```
