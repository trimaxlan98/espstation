import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react'
import type { PropsWithChildren } from 'react'
import './primitives.css'

export type ToastTone = 'ok' | 'warn' | 'crit' | 'info'

export interface ToastOptions {
  tone?: ToastTone
  title: string
  description?: string
  /** Auto-dismiss delay in ms. Default 5000. */
  durationMs?: number
}

interface ToastItem {
  id: string
  tone: ToastTone
  title: string
  description?: string
  durationMs: number
}

interface ToastContextValue {
  toasts: ToastItem[]
  show: (opts: ToastOptions) => string
  dismiss: (id: string) => void
}

const ToastContext = createContext<ToastContextValue | null>(null)

function makeId(): string {
  return typeof crypto !== 'undefined' && 'randomUUID' in crypto
    ? crypto.randomUUID()
    : `toast-${Date.now()}-${Math.random().toString(36).slice(2)}`
}

/** Holds toast state app-wide. Mount once (App.tsx), then `useToast()`/`<ToastViewport>` anywhere inside. */
export function ToastProvider({ children }: PropsWithChildren): React.JSX.Element {
  const [toasts, setToasts] = useState<ToastItem[]>([])

  const show = useCallback((opts: ToastOptions): string => {
    const id = makeId()
    const item: ToastItem = {
      id,
      tone: opts.tone ?? 'info',
      title: opts.title,
      description: opts.description,
      durationMs: opts.durationMs ?? 5000
    }
    setToasts((prev) => [...prev, item])
    return id
  }, [])

  const dismiss = useCallback((id: string): void => {
    setToasts((prev) => prev.filter((t) => t.id !== id))
  }, [])

  const value = useMemo(() => ({ toasts, show, dismiss }), [toasts, show, dismiss])

  return <ToastContext.Provider value={value}>{children}</ToastContext.Provider>
}

function useToastContext(): ToastContextValue {
  const ctx = useContext(ToastContext)
  if (!ctx) throw new Error('useToast/ToastViewport must be used within a <ToastProvider>')
  return ctx
}

/** `useToast().show({ tone, title, description? })` pushes a toast; returns its id for a manual `dismiss`. */
export function useToast(): { show: (opts: ToastOptions) => string; dismiss: (id: string) => void } {
  const { show, dismiss } = useToastContext()
  return { show, dismiss }
}

const TONE_ICON: Record<ToastTone, string> = {
  ok: '✓',
  warn: '⚠',
  crit: '✕',
  info: 'ℹ'
}

function ToastCard({ toast, onDismiss }: { toast: ToastItem; onDismiss: (id: string) => void }): React.JSX.Element {
  useEffect(() => {
    const timer = setTimeout(() => onDismiss(toast.id), toast.durationMs)
    return () => clearTimeout(timer)
  }, [toast.id, toast.durationMs, onDismiss])

  return (
    <div className={`ui-toast ui-toast--${toast.tone}`}>
      <span className="ui-toast__icon" aria-hidden="true">
        {TONE_ICON[toast.tone]}
      </span>
      <div className="ui-toast__text">
        <p className="ui-toast__title">{toast.title}</p>
        {toast.description ? <p className="ui-toast__description">{toast.description}</p> : null}
      </div>
      <button type="button" className="ui-toast__close" aria-label="Dismiss" onClick={() => onDismiss(toast.id)}>
        <svg viewBox="0 0 12 12" fill="none" aria-hidden="true">
          <path d="M2.5 2.5l7 7M9.5 2.5l-7 7" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
        </svg>
      </button>
    </div>
  )
}

/** Fixed top-right stack. Critical toasts get an `aria-live="assertive"` region; everything else is polite. */
export function ToastViewport(): React.JSX.Element {
  const { toasts, dismiss } = useToastContext()
  const critical = toasts.filter((t) => t.tone === 'crit')
  const rest = toasts.filter((t) => t.tone !== 'crit')

  return (
    <div className="ui-toast-viewport">
      <div className="ui-toast-stack" aria-live="assertive" aria-atomic="false">
        {critical.map((t) => (
          <ToastCard key={t.id} toast={t} onDismiss={dismiss} />
        ))}
      </div>
      <div className="ui-toast-stack" aria-live="polite" aria-atomic="false">
        {rest.map((t) => (
          <ToastCard key={t.id} toast={t} onDismiss={dismiss} />
        ))}
      </div>
    </div>
  )
}
