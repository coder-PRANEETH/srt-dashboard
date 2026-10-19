import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import process from 'node:process'

// Node 5 hosts the site and relays CAN sensor data through /api/*.
// Use DASH_ESP_HOST=http://192.168.4.1 for development against the board.
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

  // Root assets also resolve when opening /status/ directly.
  base: '/',

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
