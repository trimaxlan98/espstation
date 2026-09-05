import { useEffect, useRef } from 'react'
import type { EChartsType } from 'echarts/core'
import { echarts } from '../lib/echartsSetup'
import type { TelemetryPoint } from '../lib/apiTypes'

export interface ChartSeries {
  name: string
  unit: string
  data: TelemetryPoint[]
}

export interface TelemetryChartProps {
  series: ChartSeries[]
}

/** Reads the live theme tokens so the chart repaints correctly across light/dark without hard-coded colors. */
function readThemeColors(): { text: string; muted: string; border: string; palette: string[] } {
  const style = getComputedStyle(document.documentElement)
  const get = (name: string, fallback: string): string => style.getPropertyValue(name).trim() || fallback
  return {
    text: get('--text-primary', '#e9f3f2'),
    muted: get('--text-muted', '#82988f'),
    border: get('--border-subtle', '#1c2c3d'),
    palette: [
      get('--accent', '#22d3d8'),
      get('--warn-fg', '#f5c14a'),
      get('--info-fg', '#4ab8f5'),
      get('--crit-fg', '#f5544a'),
      get('--ok-fg', '#4af59a')
    ]
  }
}

/** Zoomable ECharts line chart, one series per selected channel, timestamps as epoch seconds → JS ms. */
export function TelemetryChart({ series }: TelemetryChartProps): React.JSX.Element {
  const containerRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<EChartsType | null>(null)

  useEffect(() => {
    if (!containerRef.current) return
    const chart = echarts.init(containerRef.current)
    chartRef.current = chart
    const onResize = (): void => chart.resize()
    window.addEventListener('resize', onResize)
    return () => {
      window.removeEventListener('resize', onResize)
      chart.dispose()
      chartRef.current = null
    }
  }, [])

  useEffect(() => {
    const chart = chartRef.current
    if (!chart) return
    const colors = readThemeColors()
    chart.setOption({
      backgroundColor: 'transparent',
      color: colors.palette,
      textStyle: { color: colors.text, fontFamily: 'inherit' },
      grid: { left: 56, right: 20, top: 32, bottom: 48 },
      tooltip: { trigger: 'axis' },
      legend: { top: 0, textStyle: { color: colors.muted } },
      xAxis: {
        type: 'time',
        axisLine: { lineStyle: { color: colors.border } },
        axisLabel: { color: colors.muted }
      },
      yAxis: {
        type: 'value',
        axisLine: { lineStyle: { color: colors.border } },
        splitLine: { lineStyle: { color: colors.border, opacity: 0.4 } },
        axisLabel: { color: colors.muted }
      },
      dataZoom: [{ type: 'inside' }, { type: 'slider', height: 16, bottom: 8 }],
      series: series.map((s) => ({
        name: s.unit ? `${s.name} (${s.unit})` : s.name,
        type: 'line',
        showSymbol: false,
        data: s.data.map(([ts, value]) => [ts * 1000, value])
      }))
    })
  }, [series])

  return <div ref={containerRef} className="chart-container" role="img" aria-label="Telemetry chart" />
}
