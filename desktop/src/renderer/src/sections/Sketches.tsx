/**
 * Sketches — the bench practices as files you can take to the Arduino IDE.
 *
 * The app flashes ESP-IDF firmware from the Flash section, and that is the
 * route this project is built around. This section is the other one: someone
 * who has an ESP32, the Arduino IDE and no toolchain should be able to run
 * the same practice, on the same pins, and have their board show up in the
 * Morse section like any other. So the sketches are bundled into the app
 * (lib/sketches.ts) and handed out here, with the two things people get wrong
 * afterwards: which libraries are needed (none) and the folder the IDE
 * insists on.
 *
 * Nothing here talks to the gateway or to a board — this section works with
 * no hardware, no gateway and no network, which is exactly the state someone
 * is in when they come looking for a sketch.
 */
import { useState } from 'react'
import { Badge, Button, Card, useToast } from '../components/ui'
import { REQUIREMENTS, SKETCHES, folderFor } from '../lib/sketches'
import type { Sketch } from '../lib/sketches'
import '../styles/sections.css'

function lineCount(src: string): number {
  return src.split('\n').length
}

function SketchCard({ sketch }: { sketch: Sketch }): React.JSX.Element {
  const toast = useToast()
  const [busy, setBusy] = useState(false)
  const [open, setOpen] = useState(false)

  const save = async (): Promise<void> => {
    setBusy(true)
    try {
      const res = await window.espstation.sketch.save(sketch.file, sketch.source)
      // A cancelled dialog is a decision, not a failure: saying "could not
      // save" to someone who just pressed Cancel is how an app teaches people
      // to ignore its messages.
      if (res.saved) {
        toast.show({ tone: 'ok', title: `${sketch.file} saved`, description: res.path })
      } else if (res.reason !== 'cancelled') {
        toast.show({ tone: 'crit', title: `Could not save ${sketch.file}`, description: res.reason })
      }
    } catch (err) {
      toast.show({
        tone: 'crit',
        title: `Could not save ${sketch.file}`,
        description: err instanceof Error ? err.message : String(err)
      })
    } finally {
      setBusy(false)
    }
  }

  const copy = async (): Promise<void> => {
    try {
      await window.espstation.sketch.copy(sketch.source)
      toast.show({ tone: 'ok', title: `${sketch.file} copied`, description: 'Paste it into an empty Arduino sketch.' })
    } catch (err) {
      toast.show({
        tone: 'crit',
        title: 'Could not copy',
        description: err instanceof Error ? err.message : String(err)
      })
    }
  }

  return (
    <Card
      title={sketch.title}
      subtitle={sketch.boards}
      actions={
        <>
          <Button size="sm" variant="primary" loading={busy} onClick={() => void save()}>
            Save sketch folder…
          </Button>
          <Button size="sm" onClick={() => void copy()}>
            Copy source
          </Button>
        </>
      }
    >
      <p className="section__description">{sketch.summary}</p>

      <div className="sketch-meta">
        <Badge tone="info">{sketch.practice}</Badge>
        <code className="sketch-path">{sketch.path}</code>
        <span className="sketch-lines">{lineCount(sketch.source)} lines</span>
      </div>

      <table className="sketch-pins">
        <caption>Wiring</caption>
        <tbody>
          {sketch.pins.map((p) => (
            <tr key={p.pin}>
              <th scope="row">{p.pin}</th>
              <td>{p.role}</td>
            </tr>
          ))}
        </tbody>
      </table>

      <p className="section__description sketch-folder-note">
        Saving creates <code>{folderFor(sketch)}/{sketch.file}</code> inside the folder you pick — the
        Arduino IDE only opens a sketch whose folder has the same name as the file.
      </p>

      <button type="button" className="sketch-toggle" aria-expanded={open} onClick={() => setOpen((v) => !v)}>
        {open ? 'Hide source' : 'Show source'}
      </button>
      {open ? (
        <pre className="sketch-source" tabIndex={0} aria-label={`Source of ${sketch.file}`}>
          {sketch.source}
        </pre>
      ) : null}
    </Card>
  )
}

export function Sketches(): React.JSX.Element {
  const toast = useToast()

  const copyUrl = async (): Promise<void> => {
    await window.espstation.sketch.copy(REQUIREMENTS.boardsUrl)
    toast.show({ tone: 'ok', title: 'Boards Manager URL copied' })
  }

  return (
    <div className="section">
      <div className="section__header">
        <div>
          <h1 className="section__title">Sketches</h1>
          <p className="section__description">
            The bench practices as Arduino sketches. Flash one of these instead of the ESP-IDF
            firmware and the board still shows up in the Morse section.
          </p>
        </div>
      </div>

      <Card title="Before you compile">
        <dl className="sketch-reqs">
          <dt>Libraries</dt>
          <dd>
            <strong>{REQUIREMENTS.libraries}</strong>
          </dd>
          <dt>Core</dt>
          <dd>{REQUIREMENTS.core}</dd>
          <dt>Boards Manager URL</dt>
          <dd>
            <code>{REQUIREMENTS.boardsUrl}</code>{' '}
            <Button size="sm" variant="subtle" onClick={() => void copyUrl()}>
              Copy
            </Button>
          </dd>
          <dt>Board</dt>
          <dd>{REQUIREMENTS.board}</dd>
          <dt>Serial Monitor</dt>
          <dd>{REQUIREMENTS.baud}</dd>
          <dt>Wiring</dt>
          <dd>{REQUIREMENTS.wiring}</dd>
        </dl>
      </Card>

      <div className="sketch-grid">
        {SKETCHES.map((s) => (
          <SketchCard key={s.id} sketch={s} />
        ))}
      </div>
    </div>
  )
}
