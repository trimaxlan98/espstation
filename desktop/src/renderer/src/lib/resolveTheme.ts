import type { ThemeSetting } from '@shared/types'

/** The two concrete themes styles/tokens.css actually implements (`data-theme` values). */
export type ResolvedTheme = 'dark' | 'light'

/**
 * Pure resolve: `system` follows the OS preference (`prefersDark`), `dark`/
 * `light` are explicit overrides. Kept side-effect-free and framework-free
 * so it's unit-testable without jsdom/matchMedia — see resolveTheme.test.ts.
 * components/ThemeProvider.tsx is the only caller that wires this to
 * matchMedia + the DOM.
 */
export function resolveTheme(setting: ThemeSetting, prefersDark: boolean): ResolvedTheme {
  if (setting === 'system') return prefersDark ? 'dark' : 'light'
  return setting
}
