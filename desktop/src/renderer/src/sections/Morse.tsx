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
 *
 * This section only ever reads. The boards key each other over their own
 * cable and neither one waits for the station [D-1]; closing the app stops
 * the drawing, not the practice.
 */
import { useEffect, useMemo, useRef, useState } from 'react'
import { useNodesStore } from '../store/nodesStore'
import { useStreamStore } from '../store/streamStore'
import type { EventEntry } from '../store/streamStore'
import { useNavStore } from '../store/navStore'
import { Button, Card, EmptyState, StatTile } from '../components/ui'
import { MorseWave, WAVE_WINDOWS } from '../components/MorseWave'
import { MorseCadence } from '../components/MorseCadence'
import type { WaveWindow } from '../components/MorseWave'
import { buildWaves } from '../lib/morseWave'
import type { NodeWave } from '../lib/morseWave'
import type { NodeDetail, NodeSummary } from '../lib/apiTypes'
import '../styles/sections.css'

/** The sketch adapter stamps this into HELLO's fw.version. */
const MORSE_FW = 'bench-morse-duplex'
/** A board running the real esps_morse build announces these instead. */
const MORSE_CH_PREFIX = 'morse.'
/** Characters of decoded text kept on screen. */
const TEXT_LEN = 60

/**
 * `morse.tx` and `morse.rx` are deliberately NOT here.
 *
 * They used to drive two level strips on each card, and the strips were a
 * pleasant lie: main.c publishes every channel once a second, so a dot of
 * ~100 ms is invisible to them — the firmware's own NDB comment records a
 * whole SOS going by with `morse.tx` never once sampled high. The wave in
 * components/MorseWave.tsx is built from the events instead, which carry the
 * duration the board actually measured. The two channels still exist and are
 * still what identifies an esps_morse board by its NDB; nothing on this
 * screen pretends to chart them.
 */
const CH = {
  pulse: 'morse.pulse_ms',
  gap: 'morse.gap_ms',
  symbols: 'morse.symbols',
  letters: 'morse.letters',
  unknown: 'morse.unknown',
  bounces: 'morse.bounces'
} as const

/**
 * Is this board part of the practice?
 *
 * Two kinds of board are, and they do not look alike from here. The serial
 * adapter (gateway transports/morse_sketch.py) puts `bench-morse-duplex` in
 * fw.version, so the summary alone is enough. A board flashed with the real
 * esp32dev_morse firmware reports the ordinary espstation-fw version and is
 * only recognisable by its NDB — which lives in the node *detail*. Judging by
 * fw alone made the section claim "no station attached" with two real boards
 * keying each other on the bench.
 */
export function isMorseNode(n: NodeSummary, detail?: NodeDetail | null): boolean {
  if (n.fw === MORSE_FW) return true
  return detail?.ndb?.some((c) => c.key.startsWith(MORSE_CH_PREFIX)) ?? false
}

/**
 * The text each station has received, rebuilt from its RX letter/word events.
 *
 * Only `dir === 'RX'` counts: a station also emits its own key as `TX` (the
 * echo) and `?` when the sketch line carried no direction, and letting either
 * through would show an operator their own hand as if it had come down the
 * wire. One pass over the whole rail for every station, because the event
 * rail is shared and re-scanning it once per node is pure waste.
 */
function receivedTextByNode(events: EventEntry[], nodes: NodeSummary[]): Map<number, string> {
  const out = new Map<number, string>()
  if (nodes.length === 0) return out
  const wanted = new Set(nodes.map((n) => n.node_id))
  for (const e of events) {
    if (e.node_id === null || !wanted.has(e.node_id)) continue
    if (e.data?.dir !== 'RX') continue
    let piece = ''
    if (e.code === 'morse.letter') {
      const letter = e.data.letter
      // A malformed event must not paint "[object Object]" across the card.
      piece = typeof letter === 'string' || typeof letter === 'number' ? String(letter) : ''
    } else if (e.code === 'morse.word') piece = ' '
    else if (e.code === 'morse.unknown') piece = '¿'
    if (piece === '') continue
    const prev = out.get(e.node_id) ?? ''
    // A word gap only means something between letters: a decoder that
    // repeats it must not push the text off screen with blanks.
    if (piece === ' ' && (prev === '' || prev.endsWith(' '))) continue
    out.set(e.node_id, (prev + piece).slice(-TEXT_LEN))
  }
  return out
}

/** An empty pair of lanes, so a station with no events still gets a drawing. */
const NO_WAVE: NodeWave = {
  TX: { pulses: [], letters: [], words: [], filtered: [], lastTs: null },
  RX: { pulses: [], letters: [], words: [], filtered: [], lastTs: null },
  lastTs: null
}

/** A pointer to the sketches, useful exactly when there is no board yet. */
function SketchLink(): React.JSX.Element {
  const setSection = useNavStore((s) => s.setSection)
  return (
    <Button variant="subtle" size="sm" onClick={() => setSection('sketches')}>
      Get the .ino sketch
    </Button>
  )
}

export function Morse(): React.JSX.Element {
  const nodes = useNodesStore((s) => s.nodes)
  const load = useNodesStore((s) => s.load)
  const loadDetail = useNodesStore((s) => s.loadDetail)
  const detailByNode = useNodesStore((s) => s.detailByNode)
  const version = useStreamStore((s) => s.version)
  const getLatest = useStreamStore((s) => s.getLatest)
  const events = useStreamStore((s) => s.events)
  const [windowMs, setWindowMs] = useState<WaveWindow>(10_000)

  useEffect(() => {
    void load()
  }, [load])

  // One detail fetch per node whose summary is not already conclusive, at
  // most once each: the NDB is the only place a real esps_morse board
  // identifies itself. A fetch that fails is forgotten, so the next fleet
  // change retries it instead of leaving the section blind to that node.
  const asked = useRef<Set<number>>(new Set())
  useEffect(() => {
    for (const n of nodes) {
      if (n.fw === MORSE_FW || detailByNode.has(n.node_id) || asked.current.has(n.node_id)) continue
      asked.current.add(n.node_id)
      void loadDetail(n.node_id).catch(() => {
        asked.current.delete(n.node_id)
      })
    }
  }, [nodes, detailByNode, loadDetail])

  // Online first: a registry remembers every board it has ever seen, so the
  // two that are plugged in now must not be below last week's.
  const stations = useMemo(
    () =>
      nodes
        .filter((n) => isMorseNode(n, detailByNode.get(n.node_id)))
        .sort((a, b) => Number(b.online) - Number(a.online)),
    [nodes, detailByNode]
  )

  // Kept apart from `view` on purpose: the text only changes when an event
  // arrives, and folding it into the telemetry memo re-scanned the whole
  // event rail 20 times a second per station for nothing.
  const texts = useMemo(() => receivedTextByNode(events, stations), [events, stations])

  // Same reasoning as `texts`, and the same rail: the wave is laid out once
  // per event batch and then animated by a transform, so this must NOT be
  // rebuilt on the telemetry `version` — it would run at the sample rate to
  // produce byte-identical geometry.
  const waves = useMemo(
    () => buildWaves(events, stations.map((n) => n.node_id)),
    [events, stations]
  )

  // `version` is in the deps purely to re-read the ring buffers on each new
  // sample; the getters always return the current snapshot (same trick Live
  // uses).
  const view = useMemo(
    () =>
      stations.map((n) => ({
        node: n,
        text: texts.get(n.node_id) ?? '',
        wave: waves.get(n.node_id) ?? NO_WAVE,
        symbols: getLatest(n.node_id, CH.symbols)?.[1] ?? null,
        letters: getLatest(n.node_id, CH.letters)?.[1] ?? null,
        unknown: getLatest(n.node_id, CH.unknown)?.[1] ?? null,
        bounces: getLatest(n.node_id, CH.bounces)?.[1] ?? null,
        pulse: getLatest(n.node_id, CH.pulse)?.[1] ?? null,
        gap: getLatest(n.node_id, CH.gap)?.[1] ?? null
      })),
    [stations, texts, waves, getLatest, version]
  )

  // The cross-check is a statement about ONE pair of stations: each one's
  // symbol count is what the other one keyed. With three boards attached
  // there is no pair to pick, so it says so instead of comparing the first
  // two and presenting the answer as if it covered the bench.
  //
  // The pair is taken from the ONLINE stations when exactly two are online.
  // A gateway registry keeps every node it has ever seen, so after a few
  // sessions the section legitimately holds half a dozen Morse stations of
  // which two are plugged in — and judging by the raw count refused the
  // cross-check on a bench that had exactly one pair keying. When nothing is
  // online, the whole set is used, so a finished session still shows its
  // verdict.
  const cross = useMemo(() => {
    const live = view.filter((s) => s.node.online)
    const pool = live.length === 2 ? live : view
    if (pool.length !== 2) return { kind: 'refused', n: pool.length } as const
    const [a, b] = pool
    if (!a || !b || a.symbols === null || b.symbols === null) return null
    return { kind: 'pair', a, b, matches: a.symbols === b.symbols } as const
  }, [view])
  const pair = cross?.kind === 'pair' ? cross : null

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
          action={<SketchLink />}
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
        <SketchLink />
      </div>

      <div className="morse-wave-bar">
        <span>Window</span>
        {WAVE_WINDOWS.map((w) => (
          <Button
            key={w}
            size="sm"
            variant={w === windowMs ? 'primary' : 'subtle'}
            aria-pressed={w === windowMs}
            onClick={() => setWindowMs(w)}
          >
            {w / 1000} s
          </Button>
        ))}
        <span className="morse-wave-bar__spacer" />
        {/* The shapes, not only the hues: a dot is a filled circle and a dash
            a bar, in the drawing and in this key. */}
        <span>● dot · ▬ dash · the right edge is now</span>
      </div>

      <div className="morse-grid">
        {view.map((s) => (
          <Card
            key={s.node.node_id}
            title={`${s.node.label} — receiving`}
            status={s.node.online ? 'ok' : 'warn'}
            subtitle={
              s.node.online
                ? undefined
                : 'Offline — the numbers below are the last ones this board reported'
            }
          >
            {/* role="log" so a screen reader reads the letters as they land
                instead of re-reading the whole 60-character buffer. */}
            <div className="morse-text" role="log" aria-label={`Text received by ${s.node.label}`}>
              {s.text || ' '}
            </div>
            <MorseWave wave={s.wave} station={s.node.label} windowMs={windowMs} />
            <MorseCadence lane={s.wave.TX} station={s.node.label} />
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
        {pair === null ? (
          <p className="section__description">
            {cross?.kind === 'refused' && cross.n > 2
              ? `The cross-check is about one pair of stations, and ${cross.n} Morse boards are here. ` +
                'Bring exactly two online, or detach the ones that are not in the practice: with more ' +
                'than two keys a symbol count cannot be attributed to a direction.'
              : 'Both stations have to be attached and have reported their counters before the two ' +
                'directions can be compared.'}
          </p>
        ) : (
          <p className="section__description">
            {/* The verdict is a word before it is a colour: the ok/warn hues
                are otherwise the only difference between the two sentences. */}
            <strong className="morse-verdict">{pair.matches ? 'Match.' : 'Mismatch.'}</strong>{' '}
            {pair.a.node.label} received <b>{pair.a.symbols}</b> symbols,{' '}
            {pair.b.node.label} received <b>{pair.b.symbols}</b>.{' '}
            {pair.matches ? (
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
        {/* Written after a bench run where the two boards agreed on every
            single pulse to within 1 ms and this panel still said Mismatch:
            one board had been reflashed and its counter had restarted. The
            verdict was true about the counters and false about the session,
            which is the worst kind of correct. */}
        <p className="section__description morse-note">
          Both counters run from their own board&apos;s boot and nothing here can reset them, so a
          board that rebooted — reflashed, replugged, or its port reopened — starts from zero while
          the other carries on. After that, a mismatch says the two boards have been up for
          different lengths of time and nothing about the link. Reset both by replugging them, or
          compare how much each one grows over one exchange.
        </p>
      </Card>
    </div>
  )
}
