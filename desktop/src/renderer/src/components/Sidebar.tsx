import type { SectionId } from '../store/navStore'
import { useNavStore } from '../store/navStore'
import '../styles/layout.css'

interface NavEntry {
  id: SectionId
  label: string
  icon: React.JSX.Element
}

function icon(d: string): React.JSX.Element {
  return (
    <svg viewBox="0 0 16 16" fill="none" aria-hidden="true">
      <path d={d} stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  )
}

const ENTRIES: NavEntry[] = [
  { id: 'nodes', label: 'Nodes', icon: icon('M2 4.5h12M2 8h12M2 11.5h8') },
  { id: 'live', label: 'Live', icon: icon('M2 12 5.5 6l3 4 2-3 3.5 5') },
  { id: 'experiments', label: 'Experiments', icon: icon('M6 2h4M7 2v4L3.5 12a1.5 1.5 0 0 0 1.3 2.2h6.4A1.5 1.5 0 0 0 12.5 12L9 6V2') },
  { id: 'networks', label: 'Networks', icon: icon('M8 2v3M3.5 12.5l3-3M12.5 12.5l-3-3M2 13h4M10 13h4M8 5a2 2 0 1 0 0-4 2 2 0 0 0 0 4Zm-5 10a1.5 1.5 0 1 0 0-3 1.5 1.5 0 0 0 0 3Zm10 0a1.5 1.5 0 1 0 0-3 1.5 1.5 0 0 0 0 3Z') },
  { id: 'lab', label: 'Lab', icon: icon('M6 2h4M6.5 2v4.5L3 12.8A1.4 1.4 0 0 0 4.2 15h7.6a1.4 1.4 0 0 0 1.2-2.2L9.5 6.5V2') },
  { id: 'flash', label: 'Flash', icon: icon('M9 1 3 9h4l-1 6 6-9H8Z') },
  { id: 'settings', label: 'Settings', icon: icon('M8 10a2 2 0 1 0 0-4 2 2 0 0 0 0 4Zm5.4-2a5.4 5.4 0 0 1-.1 1l1.3 1-1 1.7-1.5-.5a5.5 5.5 0 0 1-1.7 1l-.2 1.6H8.5l-.2-1.6a5.5 5.5 0 0 1-1.7-1l-1.5.5-1-1.7 1.3-1a5.4 5.4 0 0 1 0-2l-1.3-1 1-1.7 1.5.5a5.5 5.5 0 0 1 1.7-1L8.5 1h1.7l.2 1.6a5.5 5.5 0 0 1 1.7 1l1.5-.5 1 1.7-1.3 1c.07.33.1.66.1 1Z') }
]

export function Sidebar(): React.JSX.Element {
  const section = useNavStore((s) => s.section)
  const setSection = useNavStore((s) => s.setSection)

  return (
    <nav className="sidebar" aria-label="Sections">
      <div className="sidebar__brand">
        <span className="sidebar__brand-mark" aria-hidden="true">
          ES
        </span>
        <span className="sidebar__brand-name">EspStation</span>
      </div>
      <div className="sidebar__nav">
        {ENTRIES.map((entry) => (
          <button
            key={entry.id}
            type="button"
            className={`sidebar__item ${section === entry.id ? 'sidebar__item--active' : ''}`}
            aria-current={section === entry.id ? 'page' : undefined}
            onClick={() => setSection(entry.id)}
          >
            <span className="sidebar__item-icon">{entry.icon}</span>
            {entry.label}
          </button>
        ))}
      </div>
      <div className="sidebar__spacer" />
    </nav>
  )
}
