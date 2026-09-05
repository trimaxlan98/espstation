import { resolve } from 'node:path'
import react from '@vitejs/plugin-react'
import { defineConfig } from 'vitest/config'

// Most suites target the pure lib/store layer (gatewayClient, stream ring
// buffer, NDB formatting, theme resolution) and run under `node`. A narrow
// second tier — `sections/**/*.test.tsx` and `components/**/*.test.tsx` —
// renders React under jsdom for wiring-level defects a pure-function test
// can't reach. Those files opt in with Vitest's `@vitest-environment jsdom`
// pragma, keeping the node tier free of jsdom's cost and globals. Aliases mirror electron.vite.config.ts's
// `renderer`/`main` blocks so vitest (which resolves modules independently
// of the electron-vite build) can follow the same `@renderer`/`@shared`
// imports the app code uses.
export default defineConfig({
  plugins: [react()],
  test: {
    include: [
      'src/renderer/src/lib/**/*.test.ts',
      'src/renderer/src/store/**/*.test.ts',
      'src/renderer/src/components/**/*.test.tsx',
      'src/renderer/src/sections/**/*.test.tsx',
      'src/main/**/*.test.ts'
    ],
    setupFiles: ['src/renderer/src/test/setup.ts'],
    environment: 'node'
  },
  resolve: {
    alias: {
      '@renderer': resolve('src/renderer/src'),
      '@shared': resolve('src/shared')
    }
  }
})
