import { contextBridge, ipcRenderer } from 'electron'
import { IpcChannel } from '@shared/ipc'
import type { EspStationBridge } from '@shared/ipc'
import type { GatewayLogLine, SettingsPatch } from '@shared/types'

// The renderer never touches Node/Electron primitives directly — this is
// the ONLY bridge (docs/ARCHITECTURE.md invariant). HTTP/WS to the gateway
// are deliberately NOT proxied here: the gateway is localhost, so the
// renderer talks to it directly via fetch/WebSocket (lib/gatewayClient.ts).
const bridge: EspStationBridge = {
  settings: {
    get: () => ipcRenderer.invoke(IpcChannel.SettingsGet),
    set: (patch: SettingsPatch) => ipcRenderer.invoke(IpcChannel.SettingsSet, patch)
  },
  app: {
    version: () => ipcRenderer.invoke(IpcChannel.AppVersion)
  },
  gateway: {
    start: () => ipcRenderer.invoke(IpcChannel.GatewayStart),
    stop: () => ipcRenderer.invoke(IpcChannel.GatewayStop),
    restart: () => ipcRenderer.invoke(IpcChannel.GatewayRestart),
    status: () => ipcRenderer.invoke(IpcChannel.GatewayStatus),
    onLog: (cb: (line: GatewayLogLine) => void) => {
      const listener = (_event: Electron.IpcRendererEvent, payload: GatewayLogLine): void => cb(payload)
      ipcRenderer.on(IpcChannel.GatewayLog, listener)
      return () => ipcRenderer.removeListener(IpcChannel.GatewayLog, listener)
    }
  }
}

// contextIsolation is always on: the renderer must never see Node/Electron
// primitives directly, only this narrow, typed bridge.
contextBridge.exposeInMainWorld('espstation', bridge)
