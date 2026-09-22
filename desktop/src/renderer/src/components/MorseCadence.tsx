/**
 * The cadence readout: where this operator's dots and dashes actually live,
 * and how much empty room is between them.
 *
 * This is the part of the old visor that the wave alone does not replace. The
 * wave says "that one was 312 ms"; this says "your dashes run 298–330 and
 * your dots stop at 108, so there are 190 ms of nothing in between and a
 * threshold in that band is safe". That second statement is the one someone
 * steers by, and it is the one the practice's own findings are written in.
 *
 * Everything here is measured off the board's own reports — see
 * lib/morseCadence.ts, which also explains why no `letra_ms` is ever
 * suggested.
 */
import { useMemo } from 'react'
import { BAND_COMFORTABLE_MS, cadenceOf } from '../lib/morseCadence'
import type { Spread } from '../lib/morseCadence'
import type { WaveLane } from '../lib/morseWave'

const VB_W = 1000
const VB_H = 54
const AXIS_Y = 34

function ms(v: number): string {
  return `${Math.round(v)} ms`
}

function Row({ label, s }: { label: string; s: Spread | null }): React.JSX.Element {
  return (
    <>
      <dt>{label}</dt>
      <dd>
        {s === null ? (
          <span className="morse-cad__none">—</span>
        ) : (
          <>
            <b>{ms(s.median)}</b> <span className="morse-cad__range">({ms(s.min)}–{ms(s.max)}, n={s.n})</span>
          </>
        )}
      </dd>
    </>
  )
}

/**
 * Two boxes and the space between them. Not a chart of every pulse: the
 * question is where the two populations sit and whether they touch, and a
 * scatter of forty ticks answers it less clearly than two boxes do.
 */
function Bands({ dots, dashes, band }: { dots: Spread; dashes: Spread; band: number | null }): React.JSX.Element {
  // The `1` floor is not decoration: the firmware omits `ms` when it is zero,
  // so a lane of instant pulses would divide every coordinate by zero and
  // paint the strip with NaNs.
  const top = Math.max(dashes.max, dots.max, 1) * 1.1
  const x = (v: number): number => (v / top) * VB_W
  const box = (s: Spread, cls: string): React.JSX.Element => (
    <g className={cls}>
      <rect x={x(s.min)} y={AXIS_Y - 16} width={Math.max(2, x(s.max) - x(s.min))} height={16} rx={3} />
      <line x1={x(s.median)} y1={AXIS_Y - 18} x2={x(s.median)} y2={AXIS_Y + 2} />
    </g>
  )
  const overlap = band !== null && band <= 0
  return (
    <svg
      className="morse-cad__bands"
      viewBox={`0 0 ${VB_W} ${VB_H}`}
      role="img"
      aria-label={
        band === null
          ? 'Not enough symbols to compare dots and dashes yet'
          : overlap
            ? `Dots reach ${ms(dots.max)} and dashes start at ${ms(dashes.min)}: the two overlap, so no dot/dash threshold classifies all of them`
            : `Dots end at ${ms(dots.max)}, dashes start at ${ms(dashes.min)}: ${ms(band)} of empty band between them`
      }
    >
      {band !== null && band > 0 ? (
        <rect
          className="morse-cad__gap"
          x={x(dots.max)}
          y={AXIS_Y - 18}
          width={x(dashes.min) - x(dots.max)}
          height={20}
        />
      ) : null}
      {box(dots, 'morse-cad__box morse-cad__box--dot')}
      {box(dashes, 'morse-cad__box morse-cad__box--dash')}
      <line className="morse-cad__axis" x1={0} y1={AXIS_Y + 2} x2={VB_W} y2={AXIS_Y + 2} />
      <text className="morse-cad__tick" x={x(dots.median)} y={VB_H - 2}>
        dots
      </text>
      <text className="morse-cad__tick" x={x(dashes.median)} y={VB_H - 2}>
        dashes
      </text>
    </svg>
  )
}

/** `lane` is the operator's own TX lane — see the module note in morseCadence. */
export function MorseCadence({ lane, station }: { lane: WaveLane; station: string }): React.JSX.Element {
  const c = useMemo(() => cadenceOf(lane), [lane])

  if (c.sample === 0) {
    return (
      <p className="section__description morse-cad__empty">
        Cadence appears once {station} has keyed a few symbols of its own — it is measured from the
        TX echo, which is this operator&apos;s hand, never from the incoming line.
      </p>
    )
  }

  return (
    <div className="morse-cad">
      <div className="morse-cad__head">
        <span className="morse-cad__title">Cadence · your key, last {c.sample} symbols</span>
        <span className="morse-cad__wpm">{c.wpm === null ? '—' : `${c.wpm} wpm`}</span>
      </div>

      {c.dots !== null && c.dashes !== null ? <Bands dots={c.dots} dashes={c.dashes} band={c.band} /> : null}

      <dl className="morse-cad__rows">
        <Row label="dot" s={c.dots} />
        <Row label="dash" s={c.dashes} />
        <Row label="gap inside a letter" s={c.intra} />
        <Row label="gap between letters" s={c.letterGaps} />
      </dl>

      {c.band === null ? (
        <p className="morse-cad__verdict">
          Key some of both before the separation means anything: there is no band until there is a
          dot and a dash.
        </p>
      ) : c.band <= 0 ? (
        <p className="morse-cad__verdict morse-cad__verdict--bad">
          <strong>Dots and dashes overlap.</strong> Your longest dot ({ms(c.dots?.max ?? 0)}) is not
          shorter than your shortest dash ({ms(c.dashes?.min ?? 0)}), so <b>no</b> punto_raya_ms
          classifies all of these correctly. That is a hand to fix, not a number.
        </p>
      ) : c.band < BAND_COMFORTABLE_MS ? (
        <p className="morse-cad__verdict morse-cad__verdict--warn">
          <strong>Separation {ms(c.band)} — fragile.</strong> The bench calls anything under{' '}
          {BAND_COMFORTABLE_MS} ms fragile: a pulse near the line changes class on its own. Lengthen
          the dashes rather than moving the threshold.
        </p>
      ) : (
        <p className="morse-cad__verdict morse-cad__verdict--ok">
          <strong>Separation {ms(c.band)} — comfortable.</strong> On these {c.sample} symbols a
          punto_raya_ms near <b>{c.midpoint}</b> ms has the most room either side. It describes this
          sample, not a setting to copy into every session.
        </p>
      )}

      {c.gapsOverlap ? (
        <p className="morse-cad__verdict morse-cad__verdict--warn">
          <strong>Your gaps overlap too.</strong> The longest silence inside a letter (
          {ms(c.intra?.max ?? 0)}) is at least as long as the shortest one between letters (
          {ms(c.letterGaps?.min ?? 0)}), so <b>no</b> letra_ms separates them — which is exactly what
          the bench session found. Pause longer between letters; do not chase the threshold.
        </p>
      ) : null}
    </div>
  )
}
