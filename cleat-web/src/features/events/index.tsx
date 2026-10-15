// 事件日志(真实对接 ): 全局事件 store —— GlobalEventsBridge 单点写入
// (SSE /api/events 实时 + REST /api/events/history 启动灌入)。
// CS 式终端风格: 彩色级别前缀 + 时间戳 + 级别过滤 + 自动滚底, 旧→新排列。
import { useEffect, useMemo, useRef, useState } from 'react'
import { ScrollText } from 'lucide-react'
import { useEventsStore, type EventLevel } from '@/stores/events-store'
import { cn } from '@/lib/utils'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

const FILTERS: { key: 'all' | EventLevel; label: string }[] = [
  { key: 'all', label: '全部' },
  { key: 'info', label: '信息' },
  { key: 'ok', label: '成功' },
  { key: 'warn', label: '警告' },
  { key: 'err', label: '错误' },
]

const LEVEL_STYLE: Record<EventLevel, string> = {
  info: 'text-sky-400',
  ok: 'text-emerald-400',
  warn: 'text-amber-400',
  err: 'text-red-400',
}
const LEVEL_PREFIX: Record<EventLevel, string> = {
  info: '[*]',
  ok: '[+]',
  warn: '[!]',
  err: '[-]',
}

function fmtTime(ts: number): string {
  const d = new Date(ts)
  const pad = (n: number) => String(n).padStart(2, '0')
  // 带日期: 历史记录跨天(昨天的 22:34 vs 今天的 09:00), 只显示时分秒会像"时间穿越"
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

export function Events() {
  const [filter, setFilter] = useState<'all' | EventLevel>('all')
  const events = useEventsStore((s) => s.events)
  const scrollRef = useRef<HTMLDivElement>(null)

  // store 为新→旧; 终端日志习惯旧→新, 渲染时反转
  const rows = useMemo(() => {
    const filtered = filter === 'all' ? events : events.filter((e) => e.level === filter)
    return [...filtered].reverse()
  }, [events, filter])

  // 新事件自动滚底
  useEffect(() => {
    const el = scrollRef.current
    if (el) el.scrollTop = el.scrollHeight
  }, [rows.length])

  return (
    <>
      <Header fixed>
        <Search className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main className='flex h-[calc(100svh-4rem)] flex-col gap-3 overflow-hidden'>
        <div className='flex flex-wrap items-center justify-between gap-2'>
          <div>
            <h2 className='flex items-center gap-2 text-2xl font-bold tracking-tight'>
              <ScrollText className='size-5' />
              事件日志
            </h2>
            <p className='text-muted-foreground'>
              实时事件流（SSE）+ 历史 · 会话上下线 / 凭据 / 侦察 / 对抗 / 文件传输
            </p>
          </div>
          <div className='flex gap-1'>
            {FILTERS.map((f) => (
              <button
                key={f.key}
                onClick={() => setFilter(f.key)}
                className={cn(
                  'rounded-md border px-3 py-1 text-xs transition-colors',
                  filter === f.key
                    ? 'border-border bg-background text-foreground shadow-sm'
                    : 'border-transparent text-muted-foreground hover:bg-background/60'
                )}
              >
                {f.label}
              </button>
            ))}
          </div>
        </div>

        <div
          ref={scrollRef}
          className='min-h-0 flex-1 overflow-y-auto rounded-md border bg-[#09090b] p-3 font-mono text-xs leading-relaxed'
        >
          {rows.map((e) => (
            <div key={e.key} className='flex gap-2 whitespace-pre-wrap'>
              <span className={cn('shrink-0', LEVEL_STYLE[e.level])}>{LEVEL_PREFIX[e.level]}</span>
              <span className='shrink-0 text-zinc-600'>{fmtTime(e.ts)}</span>
              <span className='text-zinc-300'>{e.text}</span>
            </div>
          ))}
          {rows.length === 0 && <p className='py-8 text-center text-muted-foreground'>等待事件…</p>}
        </div>
      </Main>
    </>
  )
}
