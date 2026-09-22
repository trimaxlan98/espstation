import { resolve } from 'node:path'
import { defineConfig, externalizeDepsPlugin } from 'electron-vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  main: {
    plugins: [externalizeDepsPlugin()],
    resolve: {
      alias: {
        '@shared': resolve('src/shared')
      }
    }
  },
  preload: {
    plugins: [externalizeDepsPlugin()],
    resolve: {
      alias: {
        '@shared': resolve('src/shared')
      }
    },
    build: {
      rollupOptions: {
        output: {
          // package.json is "type": "module", so a plain .js preload bundle
          // would be parsed as ESM — Electron's sandboxed preload realm
          // cannot load ESM. Force CJS (as .cjs) so `sandbox: true` can stay
          // on the renderer (same reasoning as PiStation's preload build).
          format: 'cjs',
          entryFileNames: '[name].cjs'
        }
      }
    }
  },
  renderer: {
    resolve: {
      alias: {
        '@renderer': resolve('src/renderer/src'),
        '@shared': resolve('src/shared'),
        // The bench sketches are imported with `?raw` so the packaged app can
        // hand them out on a machine that has no copy of this repository
        // (lib/sketches.ts). The alias points OUT of desktop/ on purpose:
        // the sketch that ships must be the same file the boards are flashed
        // from, not a copy that drifts.
        '@sketches': resolve('../bench/practicas')
      }
    },
    // `electron-vite dev` serves the renderer, and its server refuses to read
    // outside the root — without this the sketches resolve in a packaged
    // build and fail only in dev, which is the wrong way round.
    server: { fs: { allow: [resolve('.'), resolve('../bench/practicas')] } },
    plugins: [react()]
  }
})
