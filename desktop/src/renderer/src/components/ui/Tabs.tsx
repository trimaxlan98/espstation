import { useRef } from 'react'
import type { KeyboardEvent } from 'react'
import './primitives.css'

export interface TabItem {
  id: string
  label: string
  badge?: string | number
}

export interface TabsProps {
  tabs: TabItem[]
  value: string
  onChange: (id: string) => void
  'aria-label'?: string
}

/** ARIA tablist with left/right arrow navigation and an animated active underline. */
export function Tabs({ tabs, value, onChange, 'aria-label': ariaLabel }: TabsProps): React.JSX.Element {
  const refs = useRef<Record<string, HTMLButtonElement | null>>({})

  function handleKeyDown(e: KeyboardEvent<HTMLDivElement>): void {
    const idx = tabs.findIndex((t) => t.id === value)
    if (idx === -1) return
    let nextIdx: number | null = null
    if (e.key === 'ArrowRight') nextIdx = (idx + 1) % tabs.length
    else if (e.key === 'ArrowLeft') nextIdx = (idx - 1 + tabs.length) % tabs.length
    else if (e.key === 'Home') nextIdx = 0
    else if (e.key === 'End') nextIdx = tabs.length - 1
    if (nextIdx === null) return
    e.preventDefault()
    const next = tabs[nextIdx]
    if (!next) return
    onChange(next.id)
    refs.current[next.id]?.focus()
  }

  return (
    <div className="ui-tabs" role="tablist" aria-label={ariaLabel} onKeyDown={handleKeyDown}>
      {tabs.map((tab) => {
        const active = tab.id === value
        return (
          <button
            key={tab.id}
            ref={(el) => {
              refs.current[tab.id] = el
            }}
            type="button"
            role="tab"
            id={`tab-${tab.id}`}
            aria-selected={active}
            aria-controls={`tabpanel-${tab.id}`}
            tabIndex={active ? 0 : -1}
            className={`ui-tabs__tab ${active ? 'ui-tabs__tab--active' : ''}`}
            onClick={() => onChange(tab.id)}
          >
            {tab.label}
            {tab.badge !== undefined ? <span className="ui-tabs__badge">{tab.badge}</span> : null}
          </button>
        )
      })}
    </div>
  )
}
