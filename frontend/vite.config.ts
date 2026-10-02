import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Mirrors SeverActions' frontend config — the settings proven to
// work when WebKit loads a Vite bundle from file:// (relative base, modern
// target, no chunk splitting). Output goes straight into views/app/, which
// build.ps1 stages into the runtime dir beside the plain-HTML views.
// Vite stamps `crossorigin` on module script/link tags; over file:/// the
// page origin is null, so CORS-mode fetches can refuse. Strip the attribute
// — same-origin file loads don't need it.
const stripCrossorigin = () => ({
  name: 'strip-crossorigin',
  transformIndexHtml(html: string) {
    return html.replace(/\scrossorigin(="[^"]*")?/g, '')
  },
})

export default defineConfig({
  plugins: [react(), stripCrossorigin()],
  base: './',
  build: {
    outDir: '../views/app',
    emptyOutDir: true,
    assetsDir: 'assets',
    sourcemap: false,
    // es2022 — SA's setting, byte-for-byte build parity. Legal since the
    // Ultralight 1.4 upgrade: WebKit 615 (Safari-16.4 era) parses the whole
    // ?-family (??=, ?., ??). The 1.3-era WebKit 610 did NOT (field log
    // 0.5.2: "Unexpected token '?'") — do not lower the SDK without
    // returning this to es2019.
    target: 'es2022',
    chunkSizeWarningLimit: 1500,
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
  },
})
