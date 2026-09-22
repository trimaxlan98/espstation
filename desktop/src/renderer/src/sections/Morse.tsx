/**
 * Morse — the full-duplex bench practice, seen from the app.
 *
 * Two boards, one crossed cable, one hand on each key. The panel shows what
 * each station is RECEIVING (decoded by the far end, which is the only thing
 * that proves something was communicated), the counters that say whether the
 * link lost anything, and the cadence numbers that say whether the operator's
 * timing is inside the thresholds.
 *
 * Everything here comes off the ordinary node surface — NDB channels and
 * events — because that is the point of the adapter: a Morse board is not a
 * special case downstream. Contract: bench/practicas/morse-duplex/SPEC-DUPLEX.md.
 */
import { useEffect, useMemo } from 'react'
import { useNodesStore } from '../store/nodesStore'
import { useStreamStore } from '../store/streamStore'
import { Card, EmptyState, StatTile } from '../components/ui'
import type { NodeSummary, TelemetryPoint } from '../lib/apiTypes'
import '../styles/sections.css'

/** The adapter stamps this into HELLO's fw.version; nothing else uses it. */
const MORSE_FW = 'bench-morse-duplex'

const CH = {
  tx: 'morse.tx',
  rx: 'morse.rx',
  pulse: 'morse.pulse_ms',
  gap: 'morse.gap_ms',
  symbols: 'morse.symbols',
  letters: 'morse.letters',
  unknown: 'morse.unknown',
  bounces: 'morse.bounces'
} as const

export function isMorseNode(n: NodeSummary): boolean {
  return n.fw === MORSE_FW
}

/** The text a station has received, rebuilt from its RX letter/word events. */
function receivedText(events: { node_id: number | null; code: string; data?: Record<string, unknown> }[], nodeId: number): string {
  let out = ''
  for (const e of events) {
    if (e.node_id !== nodeId) continue
    if (e.data?.dir !== 'RX') continue
    if (e.code === 'morse.letter') out += String(e.data?.letter ?? '')
    else if (e.code === 'morse.word') out += ' '
    else if (e.code === 'morse.unknown') out += '¿'
  }
  return out.slice(-60)
}

function Level({ series }: { series: TelemetryPoint[] }): React.JSX.Element {
  // A square wave is the honest way to draw a key: it is either down or up.
  // 120 slots of the most recent history, oldest on the left. A point is
  // [timestamp, value]; only the value matters for a level strip.
  const slots = series.slice(-120)
  if (slots.length === 0) return <div className="morse-level morse-level--empty" />
  const w = 100 / slots.length
  return (
    <div className="morse-level" role="img" aria-label="key level over time">
      {slots.map((p, i) => (
        <span
          key={i}
          className={p[1] > 0 ? 'morse-level__on' : 'morse-level__off'}
          style={{ left: `${i * w}%`, width: `${w + 0.2}%` }}
        />
      ))}
    </div>
  )
}

export function Morse(): React.JSX.Element {
  const nodes = useNodesStore((s) => s.nodes)
  const load = useNodesStore((s) => s.load)
  const version = useStreamStore((s) => s.version)
  const getSeries = useStreamStore((s) => s.getSeries)
  const getLatest = useStreamStore((s) => s.getLatest)
  const events = useStreamStore((s) => s.events)

  useEffect(() => {
    void load()
  }, [load])

  const stations = useMemo(() => nodes.filter(isMorseNode), [nodes])

  // `version` is in the deps purely to re-read the ring buffers on each new
  // sample; the getters always return the current snapshot (same trick Live
  // uses).
  const view = useMemo(
    () =>
      stations.map((n) => ({
        node: n,
        text: receivedText(events, n.node_id),
        tx: getSeries(n.node_id, CH.tx),
        rx: getSeries(n.node_id, CH.rx),
        symbols: getLatest(n.node_id, CH.symbols)?.[1] ?? null,
        letters: getLatest(n.node_id, CH.letters)?.[1] ?? null,
        unknown: getLatest(n.node_id, CH.unknown)?.[1] ?? null,
        bounces: getLatest(n.node_id, CH.bounces)?.[1] ?? null,
        pulse: getLatest(n.node_id, CH.pulse)?.[1] ?? null,
        gap: getLatest(n.node_id, CH.gap)?.[1] ?? null
      })),
    [stations, events, getSeries, getLatest, version]
  )

  const cross = useMemo(() => {
    if (view.length !== 2) return null
    const [a, b] = view
    if (!a || !b || a.symbols === null || b.symbols === null) return null
    return { a, b, matches: a.symbols === b.symbols }
  }, [view])

  if (stations.length === 0) {
    return (
      <div className="section">
        <div className="section__header">
          <h1 className="section__title">Morse</h1>
        </div>
        <EmptyState
          title="No Morse station attached"
          description={
            'Attach a board running the morse-duplex sketch (POST /api/links with kind "morse" and the port path), ' +
            'or replay a recorded bench capture with kind "morse-replay" to see the practice with no hardware at all.'
          }
        />
      </div>
    )
  }

  return (
    <div className="section">
      <div className="section__header">
        <div>
          <h1 className="section__title">Morse</h1>
          <p className="section__description">
            Full-duplex bench practice — each station decodes the other operator&apos;s hand.
          </p>
        </div>
      </div>

      <div className="morse-grid">
        {view.map((s) => (
          <Card key={s.node.node_id} title={`${s.node.label} — receiving`}>
            <div className="morse-text" aria-live="polite">
              {s.text || ' '}
            </div>
            <div className="morse-strips">
              <div>
                <span className="morse-strips__label">its key (TX)</span>
                <Level series={s.tx} />
              </div>
              <div>
                <span className="morse-strips__label">incoming (RX)</span>
                <Level series={s.rx} />
              </div>
            </div>
            <div className="morse-stats">
              <StatTile label="Symbols" value={s.symbols ?? '—'} />
              <StatTile label="Letters" value={s.letters ?? '—'} />
              <StatTile label="Unknown" value={s.unknown ?? '—'} />
              <StatTile label="Key bounces" value={s.bounces ?? '—'} />
              <StatTile label="Last pulse" value={s.pulse !== null ? `${s.pulse} ms` : '—'} />
              <StatTile label="Last gap" value={s.gap !== null ? `${s.gap} ms` : '—'} />
            </div>
          </Card>
        ))}
      </div>

      <Card title="Link integrity">
        {cross === null ? (
          <p className="section__description">
            Both stations have to be attached and have reported their counters before the two
            directions can be compared.
          </p>
        ) : (
          <p className="section__description">
            {cross.a.node.label} received <b>{cross.a.symbols}</b> symbols,{' '}
            {cross.b.node.label} received <b>{cross.b.symbols}</b>.{' '}
            {cross.matches ? (
              <span className="morse-ok">Both directions carried the same count.</span>
            ) : (
              <span className="morse-warn">
                They differ — one direction lost or invented pulses. Check `filtered` and
                `desbordes_buffer` on the boards before blaming the thresholds.
              </span>
            )}
          </p>
        )}
        <p className="section__description morse-note">
          Counts are pulses, not letters. They can match while the letters do not: that means the
          cable is fine and a threshold is wrong — see SPEC-DUPLEX.md.
        </p>
      </Card>
    </div>
  )
}
