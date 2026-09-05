/** Polls GET /api/ping on an interval so the Topbar's status reflects REST reachability even before the WS connects. */
import { gatewayClient } from './gatewaySingleton'
import { useConnectionStore } from '../store/connectionStore'

const PING_INTERVAL_MS = 5000

export function startPingPipeline(): () => void {
  let cancelled = false

  async function tick(): Promise<void> {
    try {
      const ping = await gatewayClient.ping()
      if (!cancelled) useConnectionStore.getState().setPingOk(ping)
    } catch (err) {
      if (!cancelled) useConnectionStore.getState().setPingError(err instanceof Error ? err.message : String(err))
    }
  }

  void tick()
  const timer = setInterval(() => void tick(), PING_INTERVAL_MS)

  return () => {
    cancelled = true
    clearInterval(timer)
  }
}
