import { useEffect } from 'react'
import type { PropsWithChildren } from 'react'
import { useSettingsStore } from '../store/settingsStore'
import { resolveTheme } from '../lib/resolveTheme'

const MEDIA_QUERY = '(prefers-color-scheme: dark)'

/**
 * Sets `data-theme` ("dark" | "light") on `<html>` from the user's theme
 * setting. `system` subscribes to matchMedia and re-resolves on OS-level
 * theme changes; `dark`/`light` are static overrides. Resolution logic
 * itself lives in lib/resolveTheme.ts (pure, unit-tested) — this component
 * is just the DOM/matchMedia wiring around it.
 */
export function ThemeProvider({ children }: PropsWithChildren): React.JSX.Element {
  const theme = useSettingsStore((s) => s.settings.theme)

  useEffect(() => {
    const root = document.documentElement
    const media = window.matchMedia(MEDIA_QUERY)

    const apply = (): void => {
      root.setAttribute('data-theme', resolveTheme(theme, media.matches))
    }

    apply()

    if (theme !== 'system') return
    media.addEventListener('change', apply)
    return () => media.removeEventListener('change', apply)
  }, [theme])

  return <>{children}</>
}
