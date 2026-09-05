import { create } from 'zustand'
import type { ConnectionState } from '../lib/gatewayClient'
import type { PingResponse } from '../lib/apiTypes'

interface ConnectionStoreState {
  wsState: ConnectionState
  ping: PingResponse | null
  pingError: string | null
  setWsState: (state: ConnectionState) => void
  setPingOk: (ping: PingResponse) => void
  setPingError: (message: string) => void
}

/** Tracks the WS stream's connection state and the last REST `/api/ping` outcome — the Topbar's status dot reads both. */
export const useConnectionStore = create<ConnectionStoreState>((set) => ({
  wsState: 'idle',
  ping: null,
  pingError: null,
  setWsState: (state) => set({ wsState: state }),
  setPingOk: (ping) => set({ ping, pingError: null }),
  setPingError: (message) => set({ pingError: message })
}))
