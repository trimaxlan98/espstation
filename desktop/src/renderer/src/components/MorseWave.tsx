/**
 * The square wave of the practice: 1 while the key is closed, 0 at rest, with
 * every pulse classified as the board classified it.
 *
 * This is the drawing from the single-board visor
 * (bench/practicas/clave-morse/visor/index.html, `senalSVG`) brought into the
 * app and made live. It earns its place by being the thing an operator steers
 * by: you see the dot come out as a dot, the dash as a dash, and the silence
 * growing while you decide whether the letter is finished.
 *
 * TWO LANES, ONE AXIS. TX is your own hand as the board echoed it back, RX is
 * what arrived down the cable from the other operator. They share the time
 * axis on purpose — that shared axis IS the duplex: your dots on top and
 * theirs underneath, overlapping, is the picture the practice exists to make.
 *
 * WHY IT ANIMATES WITHOUT RE-RENDERING. The wave is laid out once, in absolute
 * time units, into a group that one transform slides leftwards; the frame loop
 * writes that transform (and the silence readout) straight to the DOM. React
 * re-renders only when an event lands. Re-rendering the geometry every frame
 * was the trap the level strips already fell into once — a few hundred nodes
 * through reconciliation, sixty times a second, to move a handful of edges.
 */
import { useCallback, useEffect, useMemo, useRef } from 'react'
import type { NodeWave, WaveLane, WavePulse } from '../lib/morseWave'
import { WAVE_SPAN_MS, lastPulseEnd } from '../lib/morseWave'

/** Width of the drawing in user units; the SVG scales it to the card. */
const VB_W = 1000
/** One lane: glyph row, the rails, the letter row under them. */
const LANE_H = 84
const LANE_GAP = 12
const PAD_TOP = 6
const VB_H = PAD_TOP + LANE_H * 2 + LANE_GAP + 10
/** Inside a lane's local coordinates. */
const Y_HIGH = 18
const Y_LOW = 54

/** Windows offered by the buttons, in ms. */
export const WAVE_WINDOWS = [5000, 10_000, 30_000] as const
export type WaveWindow = (typeof WAVE_WINDOWS)[number]

function prefersReducedMotion(): boolean {
  // jsdom has no matchMedia, and neither does a renderer old enough to lack
  // it: no media query is not a request for motion to stop.
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return false
  return window.matchMedia('(prefers-reduced-motion: reduce)').matches
}

interface LaneShape {
  /** The square-wave outline, in absolute wave units. */
  path: string
  pulses: Array<{ x: number; w: number; pulse: WavePulse }>
}

/**
 * The outline and the per-pulse boxes, in absolute units.
 *
 * The path starts and ends at rest because that is what the line actually
 * does: the gaps between pulses are the message as much as the pulses are.
 */
function laneShape(lane: WaveLane, x: (t: number) => number): LaneShape {
  const pulses = lane.pulses.map((p) => {
    const x0 = x(p.t0)
    // A pulse the firmware reported with no duration still has to be visible,
    // and a hairline is honest about it being an instant.
    return { x: x0, w: Math.max(1, x(p.t1) - x0), pulse: p }
  })
  if (pulses.length === 0) return { path: '', pulses }
  const first = pulses[0]
  if (first === undefined) return { path: '', pulses }
  let d = `M${(first.x - 40).toFixed(1)} ${Y_LOW}`
  for (const p of pulses) {
    d += ` H${p.x.toFixed(1)} V${Y_HIGH} H${(p.x + p.w).toFixed(1)} V${Y_LOW}`
  }
  const last = pulses[pulses.length - 1]
  // Rest carries on well past the right edge: the line is at 0 until it is not.
  d += ` H${last === undefined ? 0 : (last.x + last.w + VB_W * 2).toFixed(1)}`
  return { path: d, pulses }
}

/**
 * One lane's moving content. Split out so React can skip it entirely when the
 * events did not change — the transform that animates it lives on the parent.
 */
function LaneContent({ lane, x, label }: { lane: WaveLane; x: (t: number) => number; label: string }): React.JSX.Element {
  const shape = useMemo(() => laneShape(lane, x), [lane, x])
  return (
    <g>
      {shape.pulses.map(({ x: px, w, pulse }, i) => (
        <rect
          key={`p${i}`}
          x={px}
          y={Y_HIGH}
          width={w}
          height={Y_LOW - Y_HIGH}
          className={pulse.sym === '.' ? 'morse-wave__fill morse-wave__fill--dot' : 'morse-wave__fill morse-wave__fill--dash'}
        >
          <title>{`${label}: ${pulse.sym === '.' ? 'dot' : 'dash'}, ${Math.round(pulse.t1 - pulse.t0)} ms`}</title>
        </rect>
      ))}
      <path d={shape.path} className="morse-wave__trace" />
      {shape.pulses.map(({ x: px, w, pulse }, i) => {
        const cx = px + w / 2
        return (
          <g key={`g${i}`}>
            {pulse.sym === '.' ? (
              <circle cx={cx} cy={Y_HIGH - 9} r={4.5} className="morse-wave__glyph morse-wave__glyph--dot" />
            ) : (
              <rect x={cx - 11} y={Y_HIGH - 13} width={22} height={8} rx={4} className="morse-wave__glyph morse-wave__glyph--dash" />
            )}
            {w >= 26 ? (
              <text x={cx} y={Y_HIGH - 18} className="morse-wave__ms">
                {Math.round(pulse.t1 - pulse.t0)}
              </text>
            ) : null}
          </g>
        )
      })}
      {lane.filtered.map((t, i) => (
        <g key={`f${i}`} className="morse-wave__filtered">
          <line x1={x(t)} y1={Y_LOW - 7} x2={x(t)} y2={Y_LOW + 7} />
          <title>{`${label}: a pulse the debounce rejected — noise on the line`}</title>
        </g>
      ))}
      {lane.words.map((t, i) => (
        <line key={`w${i}`} x1={x(t)} y1={Y_HIGH - 20} x2={x(t)} y2={Y_LOW + 24} className="morse-wave__word" />
      ))}
      {lane.letters.map((l, i) => {
        const a = x(l.from)
        const b = Math.max(x(l.to), a + 2)
        return (
          <g key={`l${i}`} className={l.ok ? 'morse-wave__letter' : 'morse-wave__letter morse-wave__letter--bad'}>
            <path d={`M${a.toFixed(1)} ${Y_LOW + 6} v5 H${b.toFixed(1)} v-5`} />
            <text x={(a + b) / 2} y={Y_LOW + 30} className="morse-wave__letter-char">
              {l.text}
            </text>
            {l.code === '' ? null : (
              <text x={(a + b) / 2} y={Y_LOW + 42} className="morse-wave__letter-code">
                {l.code}
              </text>
            )}
          </g>
        )
      })}
    </g>
  )
}

interface Props {
  wave: NodeWave
  /** Used in the accessible description and in the per-pulse tooltips. */
  station: string
  windowMs: WaveWindow
}

export function MorseWave({ wave, station, windowMs }: Props): React.JSX.Element {
  const slideRef = useRef<SVGGElement | null>(null)
  const gapRef = useRef<SVGTextElement | null>(null)
  const anchor = wave.lastTs

  // Absolute layout: every pulse is placed once, relative to an origin that
  // only moves when a new event lands. `u` is user units per millisecond.
  const u = VB_W / windowMs
  const origin = (anchor ?? 0) - WAVE_SPAN_MS
  const x = useMemo(() => (t: number) => (t - origin) * u, [origin, u])

  // Kept in a ref, not in state: the frame loop reads the newest values
  // without the effect having to be torn down and restarted on every event.
  const live = useRef({ anchor, origin, u, wave })
  live.current = { anchor, origin, u, wave }

  // The clock the drawing is placed against, and the last wall-clock reading
  // it advanced by. Both are refs and not effect-locals ON PURPOSE: an effect
  // that restarted on every event would reset them, and the clock would snap
  // to each arriving timestamp — the exact stutter it exists to avoid, since
  // an event reaches here one link latency after the edge that caused it.
  const shownRef = useRef(0)
  const prevRef = useRef(0)

  const draw = useCallback((): void => {
    const { anchor: a, origin: o, u: k, wave: w } = live.current
    if (a === null) return
    const nowPerf = performance.now()
    const dt = prevRef.current === 0 ? 0 : nowPerf - prevRef.current
    prevRef.current = nowPerf
    // Free-runs, and only ever snaps FORWARD — never back, whatever order
    // timestamps arrive in.
    shownRef.current = shownRef.current === 0 ? a : Math.max(shownRef.current + dt, a)
    const shown = shownRef.current
    const g = slideRef.current
    if (g !== null) g.setAttribute('transform', `translate(${(VB_W - (shown - o) * k).toFixed(2)} 0)`)
    const gapEl = gapRef.current
    if (gapEl !== null) {
      // TX, not RX: this number is here so the operator can time their OWN
      // letter and word gaps against the thresholds, and TX is their own hand
      // as the board echoed it back.
      const end = lastPulseEnd(w.TX)
      gapEl.textContent = end === null ? '' : `your silence ${Math.round(Math.max(0, shown - end))} ms`
    }
  }, [])

  // The loop, started once. Reduced motion gets no loop at all; the effect
  // below still repaints it whenever the data changes, so the drawing is
  // correct, just not in continuous motion.
  useEffect(() => {
    if (prefersReducedMotion()) return undefined
    let raf = 0
    const loop = (): void => {
      draw()
      raf = requestAnimationFrame(loop)
    }
    raf = requestAnimationFrame(loop)
    return () => cancelAnimationFrame(raf)
  }, [draw])

  // One paint on every data or window change, so the first event and every
  // reduced-motion update land without waiting for a frame that may never
  // come.
  useEffect(() => {
    draw()
  }, [draw, anchor, windowMs, wave])

  const empty = anchor === null

  return (
    <div className="morse-wave">
      <svg
        viewBox={`0 0 ${VB_W} ${VB_H}`}
        role="img"
        aria-label={
          empty
            ? `${station}: no symbols keyed yet — the wave draws a pulse as soon as a key is released`
            : `${station}: square wave of the last ${Math.round(windowMs / 1000)} seconds, TX is this board's own key and RX is the incoming line`
        }
      >
        {/* The rails and the "now" edge are static: only the wave slides. */}
        {([0, 1] as const).map((i) => {
          const y = PAD_TOP + i * (LANE_H + LANE_GAP)
          return (
            <g key={i} transform={`translate(0 ${y})`}>
              <line x1={0} y1={Y_LOW} x2={VB_W} y2={Y_LOW} className="morse-wave__rail" />
              <line x1={0} y1={Y_HIGH} x2={VB_W} y2={Y_HIGH} className="morse-wave__rail morse-wave__rail--high" />
              <text x={6} y={Y_HIGH - 4} className="morse-wave__lane-label">
                {i === 0 ? 'TX · your key' : 'RX · incoming'}
              </text>
            </g>
          )
        })}

        <g ref={slideRef}>
          <g transform={`translate(0 ${PAD_TOP})`}>
            <LaneContent lane={wave.TX} x={x} label={`${station} TX`} />
          </g>
          <g transform={`translate(0 ${PAD_TOP + LANE_H + LANE_GAP})`}>
            <LaneContent lane={wave.RX} x={x} label={`${station} RX`} />
          </g>
        </g>

        <line x1={VB_W} y1={0} x2={VB_W} y2={VB_H} className="morse-wave__now" />
        <text ref={gapRef} x={VB_W - 8} y={VB_H - 2} className="morse-wave__gap" />
        {empty ? (
          <text x={VB_W / 2} y={VB_H / 2} className="morse-wave__idle">
            waiting for the first symbol
          </text>
        ) : null}
      </svg>
    </div>
  )
}
