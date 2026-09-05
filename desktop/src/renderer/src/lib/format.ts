/** Small display-formatting helpers shared by Nodes/Live — no NDB knowledge here (see channelFormat.ts for that). */

export function formatUptime(ms: number): string {
  const totalSeconds = Math.floor(ms / 1000)
  const days = Math.floor(totalSeconds / 86400)
  const hours = Math.floor((totalSeconds % 86400) / 3600)
  const minutes = Math.floor((totalSeconds % 3600) / 60)
  const seconds = totalSeconds % 60
  if (days > 0) return `${days}d ${hours}h`
  if (hours > 0) return `${hours}h ${minutes}m`
  if (minutes > 0) return `${minutes}m ${seconds}s`
  return `${seconds}s`
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

/** `ts` is a float Unix epoch seconds value (the gateway's one conversion point, PROTOCOL.md §4.11). */
export function formatRelativeTime(ts: number, now: number = Date.now() / 1000): string {
  const deltaSeconds = Math.max(0, now - ts)
  if (deltaSeconds < 5) return 'just now'
  if (deltaSeconds < 60) return `${Math.floor(deltaSeconds)}s ago`
  if (deltaSeconds < 3600) return `${Math.floor(deltaSeconds / 60)}m ago`
  if (deltaSeconds < 86400) return `${Math.floor(deltaSeconds / 3600)}h ago`
  return `${Math.floor(deltaSeconds / 86400)}d ago`
}
