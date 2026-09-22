// @vitest-environment jsdom

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import { ToastProvider, ToastViewport } from '../components/ui'
import { SKETCHES } from '../lib/sketches'
import { Sketches } from './Sketches'

const save = vi.fn()
const copy = vi.fn()

beforeEach(() => {
  save.mockResolvedValue({ saved: true, path: 'C:\\tmp\\transceptor\\transceptor.ino' })
  copy.mockResolvedValue(undefined)
  // The section only ever reaches the filesystem through the bridge, so a
  // stub of the bridge is the whole boundary.
  ;(window as unknown as { espstation: unknown }).espstation = { sketch: { save, copy } }
})

afterEach(() => {
  cleanup()
  vi.clearAllMocks()
})

function draw(): ReturnType<typeof render> {
  return render(
    <ToastProvider>
      <Sketches />
      {/* The provider holds the toasts; the viewport is what draws them, and
          App.tsx mounts both. A test that left it out would be testing a
          screen the user never sees. */}
      <ToastViewport />
    </ToastProvider>
  )
}

describe('the sketch catalogue', () => {
  it('ships the real sketch text, not a placeholder', () => {
    // If the `?raw` alias ever stops resolving, this is what catches it: the
    // build would otherwise succeed and the app would hand out empty files.
    for (const s of SKETCHES) {
      expect(s.source).toContain('#include <Arduino.h>')
      expect(s.source.length).toBeGreaterThan(2000)
    }
  })

  it('names a file the Arduino IDE will accept', () => {
    for (const s of SKETCHES) expect(s.file).toMatch(/^[A-Za-z0-9_-]+\.ino$/)
  })
})

describe('Sketches section', () => {
  it('states plainly that no library has to be installed', () => {
    draw()
    expect(screen.getByText(/No third-party library is used/)).toBeTruthy()
  })

  it('warns about the pin that bricks the boot', () => {
    draw()
    expect(screen.getByText(/GPIO12 is forbidden/)).toBeTruthy()
  })

  it('offers every sketch with a save and a copy', () => {
    draw()
    expect(screen.getAllByText('Save sketch folder…').length).toBe(SKETCHES.length)
    expect(screen.getAllByText('Copy source').length).toBe(SKETCHES.length)
  })

  it('hands the file name and the source to the bridge, and says where it landed', async () => {
    draw()
    const first = SKETCHES[0]
    expect(first).toBeDefined()
    fireEvent.click(screen.getAllByText('Save sketch folder…')[0] as HTMLElement)
    await waitFor(() => expect(save).toHaveBeenCalledWith(first?.file, first?.source))
    expect(await screen.findByText(`${first?.file} saved`)).toBeTruthy()
    expect(screen.getByText('C:\\tmp\\transceptor\\transceptor.ino')).toBeTruthy()
  })

  it('stays quiet when the user cancels the dialog', async () => {
    save.mockResolvedValue({ saved: false, reason: 'cancelled' })
    draw()
    fireEvent.click(screen.getAllByText('Save sketch folder…')[0] as HTMLElement)
    await waitFor(() => expect(save).toHaveBeenCalled())
    // Cancelling is a decision, not a failure worth a red toast.
    expect(screen.queryByText(/Could not save/)).toBeNull()
  })

  it('says so when the save really did fail', async () => {
    save.mockRejectedValue(new Error('EACCES: permission denied'))
    draw()
    fireEvent.click(screen.getAllByText('Save sketch folder…')[0] as HTMLElement)
    expect(await screen.findByText(/EACCES/)).toBeTruthy()
  })

  it('copies the source through the bridge, not through navigator.clipboard', async () => {
    draw()
    const first = SKETCHES[0]
    fireEvent.click(screen.getAllByText('Copy source')[0] as HTMLElement)
    await waitFor(() => expect(copy).toHaveBeenCalledWith(first?.source))
  })

  it('keeps the source folded away until it is asked for', () => {
    const { container } = draw()
    expect(container.querySelectorAll('.sketch-source').length).toBe(0)
    const toggle = screen.getAllByText('Show source')[0] as HTMLElement
    expect(toggle.getAttribute('aria-expanded')).toBe('false')
    fireEvent.click(toggle)
    expect(container.querySelectorAll('.sketch-source').length).toBe(1)
  })

  it('tells the reader which folder the file has to end up in', () => {
    draw()
    const card = screen.getByText('Transceptor full duplex').closest('section')
    expect(card).not.toBeNull()
    expect(within(card as HTMLElement).getByText('transceptor/transceptor.ino')).toBeTruthy()
  })
})
