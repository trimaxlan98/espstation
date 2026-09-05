import { useEffect } from 'react'
import { Sidebar } from './components/Sidebar'
import { Topbar } from './components/Topbar'
import { ThemeProvider } from './components/ThemeProvider'
import { ToastProvider, ToastViewport } from './components/ui'
import { Nodes } from './sections/Nodes'
import { Live } from './sections/Live'
import { Experiments } from './sections/Experiments'
import { Networks } from './sections/Networks'
import { Lab } from './sections/Lab'
import { Flash } from './sections/Flash'
import { Settings } from './sections/Settings'
import { useNavStore } from './store/navStore'
import { useSettingsStore } from './store/settingsStore'
import { startStreamPipeline } from './lib/streamPipeline'
import { startPingPipeline } from './lib/pingPipeline'
import './styles/layout.css'

function ActiveSection(): React.JSX.Element {
  const section = useNavStore((s) => s.section)
  switch (section) {
    case 'nodes':
      return <Nodes />
    case 'live':
      return <Live />
    case 'experiments':
      return <Experiments />
    case 'networks':
      return <Networks />
    case 'lab':
      return <Lab />
    case 'flash':
      return <Flash />
    case 'settings':
      return <Settings />
  }
}

export function App(): React.JSX.Element {
  const load = useSettingsStore((s) => s.load)

  useEffect(() => {
    void load()
  }, [load])

  // WS ingestion and REST reachability polling run app-wide, independent of
  // which section is mounted, so telemetry keeps accumulating and the
  // Topbar's status stays live while looking at another section.
  useEffect(() => startStreamPipeline(), [])
  useEffect(() => startPingPipeline(), [])

  return (
    <ThemeProvider>
      <ToastProvider>
        <div className="app-shell">
          <Topbar />
          <Sidebar />
          <main className="app-content">
            <ActiveSection />
          </main>
        </div>
        <ToastViewport />
      </ToastProvider>
    </ThemeProvider>
  )
}
