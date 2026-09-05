import { useState } from 'react'
import type { ReactNode } from 'react'
import { Dialog } from './Dialog'
import { Button } from './Button'

export interface ConfirmDialogProps {
  open: boolean
  onClose: () => void
  /** Performs the mutating action. May be async — the dialog shows a loading state and stays open until it resolves. */
  onConfirm: () => void | Promise<void>
  title: string
  description: ReactNode
  confirmLabel?: string
  cancelLabel?: string
  /** True for anything that erases data, reboots, or otherwise cannot be undone from the UI. */
  danger?: boolean
}

/**
 * The one and only path to a mutating action in this app (docs/ARCHITECTURE.md:
 * "every mutating action is confirmed in the UI" — flash, erase, reboot,
 * detach, EXP_SET). Every section wires its destructive/irreversible buttons
 * through this component rather than calling the gateway directly, so the
 * confirm step can never be silently skipped by a future edit.
 */
export function ConfirmDialog({
  open,
  onClose,
  onConfirm,
  title,
  description,
  confirmLabel = 'Confirm',
  cancelLabel = 'Cancel',
  danger = true
}: ConfirmDialogProps): React.JSX.Element {
  const [busy, setBusy] = useState(false)

  async function handleConfirm(): Promise<void> {
    setBusy(true)
    try {
      await onConfirm()
      onClose()
    } finally {
      setBusy(false)
    }
  }

  return (
    <Dialog
      open={open}
      onClose={busy ? () => undefined : onClose}
      title={title}
      danger={danger}
      footer={
        <>
          <Button variant="ghost" onClick={onClose} disabled={busy}>
            {cancelLabel}
          </Button>
          <Button variant={danger ? 'danger' : 'primary'} onClick={() => void handleConfirm()} loading={busy}>
            {confirmLabel}
          </Button>
        </>
      }
    >
      {description}
    </Dialog>
  )
}
