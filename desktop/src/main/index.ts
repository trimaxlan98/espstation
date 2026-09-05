import { app, BrowserWindow, ipcMain, shell } from 'electron'
import { join } from 'node:path'
import { is } from './isDev'
import { loadSettings, saveSettings } from './settings'
import { GatewaySupervisor } from './gatewaySupervisor'
import { IpcChannel } from '@shared/ipc'
import type { GatewayLogLine, SettingsPatch } from '@shared/types'

// Some Linux setups (notably Wayland dev sandboxes without a configured user
// namespace) refuse Chromium's sandbox. Only disable it in dev, never in a
// packaged build.
if (is.dev && process.platform === 'linux') {
  app.commandLine.appendSwitch('no-sandbox')
  // Headless/CI-style Linux dev boxes frequently lack a usable VA-API/GPU
  // stack *and* a working GPU-process sandbox, which crashes the GPU
  // process fatally a few seconds after launch, taking the whole app down.
  // disableHardwareAcceleration() alone still spawns (and can crash) a GPU
  // process for compositing, so also force plain --disable-gpu to keep
  // Chromium off that path entirely — ECharts' canvases still render fine.
  app.disableHardwareAcceleration()
  app.commandLine.appendSwitch('disable-gpu')
  app.commandLine.appendSwitch('disable-software-rasterizer')
  app.commandLine.appendSwitch('disable-gpu-compositing')
}

function createWindow(): BrowserWindow {
  const win = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 1024,
    minHeight: 680,
    show: false,
    backgroundColor: '#0a1420',
    autoHideMenuBar: true,
    webPreferences: {
      // Electron's sandboxed preload realm cannot load an ESM bundle
      // ("Cannot use import statement outside a module") — package.json
      // is "type": "module", so the preload is force-built as CJS
      // (index.cjs, see electron.vite.config.ts) specifically so
      // `sandbox: true` can stay on.
      preload: join(__dirname, '../preload/index.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  })

  win.on('ready-to-show', () => win.show())

  win.webContents.setWindowOpenHandler((details) => {
    void shell.openExternal(details.url)
    return { action: 'deny' }
  })

  // Never allow the top-level frame to navigate away from the app itself
  // (dev server or the packaged file:// entry point).
  win.webContents.on('will-navigate', (event, url) => {
    const devUrl = process.env['ELECTRON_RENDERER_URL']
    const allowed = (devUrl && url.startsWith(devUrl)) || url.startsWith('file://')
    if (!allowed) event.preventDefault()
  })

  // Dev-only: relay renderer console messages to main stdout as
  // `[renderer:LEVEL] ...` so the orchestrator can verify console cleanliness
  // headlessly. The gateway bearer token travels in WS URLs (?token=...) by
  // contract and Chromium logs failed WS URLs verbatim — redact before relaying.
  if (is.dev || process.env['ESPSTATION_DIAG'] === '1') {
    win.webContents.on('console-message', (event) => {
      const { level, message, sourceId, lineNumber } = event
      const redacted = message.replace(/([?&]token=)[^&'" ]+/gi, '$1<redacted>')
      // eslint-disable-next-line no-console
      console.log(`[renderer:${level.toUpperCase()}] ${redacted} (${sourceId}:${lineNumber})`)
    })
    win.webContents.on('did-fail-load', (_e, code, desc, url) => {
      // eslint-disable-next-line no-console
      console.log(`[did-fail-load] code=${code} desc=${desc} url=${url}`)
    })
    win.webContents.on('render-process-gone', (_e, details) => {
      // eslint-disable-next-line no-console
      console.log(`[render-process-gone] ${JSON.stringify(details)}`)
    })
  }

  if (is.dev && process.env['ELECTRON_RENDERER_URL']) {
    void win.loadURL(process.env['ELECTRON_RENDERER_URL'])
  } else {
    void win.loadFile(join(__dirname, '../renderer/index.html'))
  }

  return win
}

let gateway: GatewaySupervisor | null = null

function broadcastGatewayLog(line: GatewayLogLine): void {
  // Never logged verbatim: the token is a Settings field, not something the
  // supervisor's own log lines ever interpolate.
  for (const win of BrowserWindow.getAllWindows()) {
    win.webContents.send(IpcChannel.GatewayLog, line)
  }
  if (is.dev) {
    // eslint-disable-next-line no-console
    console.log(`[gateway:${line.stream}:${line.level}] ${line.message}`)
  }
}

function registerIpcHandlers(): void {
  ipcMain.handle(IpcChannel.SettingsGet, () => loadSettings())

  ipcMain.handle(IpcChannel.SettingsSet, (_event, patch: SettingsPatch) => saveSettings(patch))

  ipcMain.handle(IpcChannel.AppVersion, () => app.getVersion())

  ipcMain.handle(IpcChannel.GatewayStart, () => {
    if (!gateway) throw new Error('gateway supervisor not initialized')
    const settings = loadSettings()
    return gateway.start(settings.gatewayMode, settings.gatewayPythonPath)
  })

  ipcMain.handle(IpcChannel.GatewayStop, () => {
    if (!gateway) throw new Error('gateway supervisor not initialized')
    return gateway.stop()
  })

  ipcMain.handle(IpcChannel.GatewayRestart, () => {
    if (!gateway) throw new Error('gateway supervisor not initialized')
    const settings = loadSettings()
    return gateway.restart(settings.gatewayPythonPath)
  })

  ipcMain.handle(IpcChannel.GatewayStatus, () => {
    if (!gateway) throw new Error('gateway supervisor not initialized')
    return gateway.status()
  })
}

void app.whenReady().then(() => {
  gateway = new GatewaySupervisor({ emitLog: broadcastGatewayLog })
  registerIpcHandlers()
  createWindow()

  // Bring the gateway up automatically on launch — the whole point of
  // 'managed' mode is that the operator doesn't run a second terminal.
  // 'attach' mode's start() is a cheap no-op (see gatewaySupervisor.ts).
  const settings = loadSettings()
  gateway.start(settings.gatewayMode, settings.gatewayPythonPath)

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})

// The gateway's shutdown is async (SIGTERM, then a SIGKILL grace period), so
// it can't complete inside a plain `before-quit` handler synchronously.
// Guarded by `quitting` so the app.quit() call below — which re-fires
// `before-quit` — doesn't loop forever.
let quitting = false
app.on('before-quit', (event) => {
  if (quitting) return
  quitting = true
  event.preventDefault()
  const done = gateway ? gateway.shutdown() : Promise.resolve()
  void done.finally(() => app.quit())
})
