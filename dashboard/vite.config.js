import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

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
     */
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/app.js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },

  server: {
    proxy: {
      '/api': { target: ESP_HOST, changeOrigin: true },
    },
  },
})
