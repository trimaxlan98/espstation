import { describe, expect, it } from 'vitest'
import { resolveTheme } from './resolveTheme'

describe('resolveTheme', () => {
  it('follows the OS preference when set to "system"', () => {
    expect(resolveTheme('system', true)).toBe('dark')
    expect(resolveTheme('system', false)).toBe('light')
  })

  it('an explicit "dark" override ignores the OS preference', () => {
    expect(resolveTheme('dark', false)).toBe('dark')
  })

  it('an explicit "light" override ignores the OS preference', () => {
    expect(resolveTheme('light', true)).toBe('light')
  })
})
