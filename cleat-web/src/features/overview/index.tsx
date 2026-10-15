// Cleat 总览页(原型 P2+) —— 展示"那味"的核心页面:
// KPI 卡片 ×4 + 实时事件流(SSE 全局) + 权限分布(环形图) + 会话速览。
import { useEffect, useMemo, useRef } from 'react'
import { useQuery } from '@tanstack/react-query'
import { Link, useNavigate } from '@tanstack/react-router'
import { Cell, Pie, PieChart, ResponsiveContainer, Tooltip } from 'recharts'
import {
  Activity,
  ArrowUpRight,
  ChevronRight,
  MonitorCheck,
  Radio,
  ShieldAlert,
  Waypoints,
} from 'lucide-react'
import { listSessions, type SessionRow } from '@/lib/cleat-api'
import { listListeners } from '@/lib/cleat-rest'
import { cn } from '@/lib/utils'
import { useEventsStore } from '@/stores/events-store'
import { sessionFnTab, useTabsStore } from '@/stores/tabs-store'
import { Badge } from '@/components/ui/badge'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/skeleton'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

const PIE_COLORS = ['#10b981', '#f59e0b', '#ef4444', '#38bdf8', '#a78bfa']

function fmtRelative(ts: number): string {
  const diff = Date.now() - ts
  if (diff < 10_000) return '刚刚'
  if (diff < 60_000) return `${Math.floor(diff / 1000)} 秒前`
  if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} 分钟前`
  if (diff < 86_400_000) return `${Math.floor(diff / 3_600_000)} 小时前`
  return `${Math.floor(diff / 86_400_000)} 天前`
}

function isHighPriv(p: string): boolean {
  const s = (p || '').toLowerCase()
  return s.includes('system') || s.includes('admin') || s.includes('high')
}

// ---- 实时事件流 ----
// 数据源: 全局事件 store(GlobalEventsBridge 单点写入: SSE /api/events + 历史灌入 + 演示模拟)。
// 本组件只做展示 + 兜底 diff(真实模式: TS 防抖窗口内 SSE 重连不发事件, 用 sessions 状态变化保证感知)。
function LiveFeed({ sessions }: { sessions: SessionRow[] }) {
  const events = useEventsStore((s) => s.events)
  const push = useEventsStore((s) => s.push)
  const prevOnlineRef = useRef<Map<number, boolean> | null>(null)

  // 兜底 diff(仅真实模式): 会话列表状态变化 → 上下线感知。
  // 注意: 不做"初始播种"(把所有会话报一遍在线/离线) —— 每次进总览都会重复推一遍同样的行,
  // 且与历史事件灌入重叠; 历史上下文由 GlobalEventsBridge 的 history 灌入负责。
  useEffect(() => {
    if (sessions.length === 0 || prevOnlineRef.current === null) {
      if (sessions.length > 0) {
        prevOnlineRef.current = new Map(sessions.map((s) => [s.id, s.online]))
      }
      return
    }
    const prev = prevOnlineRef.current
    const next = new Map(sessions.map((s) => [s.id, s.online]))
    for (const [id, online] of next) {
      const was = prev.get(id)
      if (was === undefined) {
        push(online ? 'ok' : 'err', `会话 #${id} 上线`)
      } else if (was !== online) {
        push(online ? 'ok' : 'err', `会话 #${id} ${online ? '恢复在线' : '离线'}`)
      }
    }
    // 会话从列表中消失(TS 离线后内存摘除) → 视为离线
    for (const [id, was] of prev) {
      if (was && !next.has(id)) {
        push('err', `会话 #${id} 离线`)
      }
    }
    prevOnlineRef.current = next
  }, [sessions, push])

  return (
    <Card className='flex h-full flex-col'>
      <CardHeader className='pb-3'>
        <CardTitle className='flex items-center gap-2 text-base'>
          <Radio className='size-4 text-emerald-500' />
          实时动态
        </CardTitle>
        <CardDescription>会话上下线与关键事件（SSE 实时 + 轮询对账）</CardDescription>
      </CardHeader>
      <CardContent className='min-h-0 flex-1 overflow-y-auto'>
        {events.length === 0 ? (
          <div className='flex h-40 flex-col items-center justify-center gap-1 text-sm text-muted-foreground'>
            <Activity className='size-5 opacity-40' />
            等待事件…（Agent 上线 / 下线将实时出现）
          </div>
        ) : (
          <ul className='space-y-1'>
            {events.map((e) => (
              <li
                key={e.key}
                className='flex items-center gap-2.5 rounded-md px-2 py-1.5 text-sm transition-colors hover:bg-muted/60'
              >
                <span
                  className={cn(
                    'inline-block size-2 shrink-0 rounded-full',
                    e.level === 'ok'
                      ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]'
                      : e.level === 'err'
                        ? 'bg-red-500/70'
                        : e.level === 'warn'
                          ? 'bg-amber-400'
                          : 'bg-sky-500/70'
                  )}
                />
                <span className='truncate'>{e.text}</span>
                <span className='ms-auto shrink-0 text-xs text-muted-foreground'>{fmtRelative(e.ts)}</span>
              </li>
            ))}
          </ul>
        )}
      </CardContent>
    </Card>
  )
}

export function OverviewPage() {
  const navigate = useNavigate()
  const openTab = useTabsStore((s) => s.openTab)
  const { data, isLoading, dataUpdatedAt } = useQuery({
    queryKey: ['sessions'],
    queryFn: listSessions,
    refetchInterval: 3000,
  })
  const sessions: SessionRow[] = data?.sessions ?? []

  const stats = useMemo(() => {
    const online = sessions.filter((s) => s.online)
    const highPriv = sessions.filter((s) => isHighPriv(s.privilege))
    const transports = new Set(sessions.map((s) => s.transport).filter(Boolean))
    // "1 分钟内活跃"以本轮数据刷新时刻为基准(dataUpdatedAt 渲染期稳定, 随 3s 轮询推进)
    const active1m = sessions.filter((s) => dataUpdatedAt - s.last_seen < 60_000)
    return { online: online.length, total: sessions.length, highPriv: highPriv.length, transports: transports.size, active1m: active1m.length }
  }, [sessions, dataUpdatedAt])

  // 平台分布(对齐老版仪表盘): Windows / Linux / macOS
  const platformDist = useMemo(() => {
    const m = new Map<string, number>()
    for (const s of sessions) {
      const k = /windows/i.test(s.os) ? 'Windows' : /linux|ubuntu|kali|amazon/i.test(s.os) ? 'Linux' : /macos|darwin/i.test(s.os) ? 'macOS' : '其他'
      m.set(k, (m.get(k) || 0) + 1)
    }
    return [...m.entries()].map(([name, value]) => ({ name, value }))
  }, [sessions])

  // 监听器 KPI: 真实数据(与监听器页同缓存)
  const listenersQ = useQuery({ queryKey: ['listeners'], queryFn: listListeners, refetchInterval: 10000 })
  const listenerStats = {
    running: (listenersQ.data?.listeners ?? []).filter((l) => l.status === 'running').length,
    total: listenersQ.data?.listeners.length ?? 0,
  }

  const cards = [
    {
      title: '在线会话',
      value: stats.online,
      desc: `共 ${stats.total} 个会话` + (stats.active1m ? ` · ${stats.active1m} 个活跃` : ''),
      icon: Activity,
      tone: 'text-emerald-500',
    },
    {
      title: '高权限会话',
      value: stats.highPriv,
      desc: 'SYSTEM / Admin / High',
      icon: ShieldAlert,
      tone: 'text-orange-400',
    },
    {
      title: '监听器',
      value: listenerStats.running,
      desc: `运行中 / 共 ${listenerStats.total} 个`,
      icon: Radio,
      tone: 'text-sky-400',
    },
    {
      title: '通道类型',
      value: stats.transports,
      desc: 'TCP / HTTPS / WSS / QUIC',
      icon: Waypoints,
      tone: 'text-violet-400',
    },
  ]

  return (
    <>
      <Header fixed>
        <Search className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>

      <Main className='flex flex-1 flex-col gap-4 sm:gap-6'>
        <div className='flex flex-wrap items-end justify-between gap-2'>
          <div>
            <h2 className='text-2xl font-bold tracking-tight'>总览</h2>
            <p className='text-muted-foreground'>实时作战态势 · 每 3 秒自动刷新</p>
          </div>
          <Badge variant='outline' className='gap-1 border-emerald-500/40 text-emerald-500'>
            <span className='inline-block size-1.5 rounded-full bg-emerald-500' />
            链路正常
          </Badge>
        </div>

        {/* KPI 卡片 */}
        <div className='grid gap-4 sm:grid-cols-2 lg:grid-cols-4'>
          {cards.map((c) => (
            <Card key={c.title} className='gap-2 py-4'>
              <CardHeader className='flex flex-row items-center justify-between space-y-0 pb-0'>
                <CardTitle className='text-sm font-medium text-muted-foreground'>{c.title}</CardTitle>
                <c.icon className={cn('size-4', c.tone)} />
              </CardHeader>
              <CardContent>
                <div className='text-2xl font-bold tracking-tight'>{isLoading ? '—' : c.value}</div>
                <p className='mt-0.5 text-xs text-muted-foreground'>{c.desc}</p>
              </CardContent>
            </Card>
          ))}
        </div>

        {/* 实时动态 + 平台分布 */}
        <div className='grid gap-4 lg:grid-cols-3'>
          <div className='lg:col-span-2'>
            <LiveFeed sessions={sessions} />
          </div>
          <Card className='flex flex-col'>
            <CardHeader className='pb-3'>
              <CardTitle className='text-base'>平台分布</CardTitle>
              <CardDescription>按 Agent 操作系统统计</CardDescription>
            </CardHeader>
            <CardContent className='flex min-h-0 flex-1 items-center justify-center'>
              {platformDist.length === 0 ? (
                <span className='text-sm text-muted-foreground'>暂无数据</span>
              ) : (
                <div className='relative h-48 w-full'>
                  <ResponsiveContainer width='100%' height='100%'>
                    <PieChart>
                      <Pie
                        data={platformDist}
                        dataKey='value'
                        nameKey='name'
                        innerRadius={52}
                        outerRadius={72}
                        paddingAngle={3}
                        strokeWidth={0}
                      >
                        {platformDist.map((_, i) => (
                          <Cell key={i} fill={PIE_COLORS[i % PIE_COLORS.length]} />
                        ))}
                      </Pie>
                      <Tooltip
                        contentStyle={{
                          background: 'hsl(var(--popover))',
                          border: '1px solid hsl(var(--border))',
                          borderRadius: 8,
                          fontSize: 12,
                        }}
                      />
                    </PieChart>
                  </ResponsiveContainer>
                  <div className='pointer-events-none absolute inset-0 flex flex-col items-center justify-center'>
                    <span className='text-2xl font-bold'>{sessions.length}</span>
                    <span className='text-xs text-muted-foreground'>会话</span>
                  </div>
                  <div className='mt-1 flex flex-wrap justify-center gap-x-4 gap-y-1'>
                    {platformDist.map((d, i) => (
                      <span key={d.name} className='flex items-center gap-1.5 text-xs text-muted-foreground'>
                        <span
                          className='inline-block size-2 rounded-full'
                          style={{ background: PIE_COLORS[i % PIE_COLORS.length] }}
                        />
                        {d.name} · {d.value}
                      </span>
                    ))}
                  </div>
                </div>
              )}
            </CardContent>
          </Card>
        </div>

        {/* 会话速览 */}
        <Card>
          <CardHeader className='flex flex-row items-center justify-between pb-3'>
            <div className='space-y-1'>
              <CardTitle className='text-base'>会话速览</CardTitle>
              <CardDescription>点击进入终端 / 文件 / 进程</CardDescription>
            </div>
            <Link to='/sessions' className='flex items-center gap-1 text-sm text-muted-foreground hover:text-foreground'>
              全部会话
              <ArrowUpRight className='size-3.5' />
            </Link>
          </CardHeader>
          <CardContent>
            {isLoading ? (
              <div className='space-y-2'>
                {Array.from({ length: 3 }).map((_, i) => (
                  <Skeleton key={i} className='h-10 w-full' />
                ))}
              </div>
            ) : sessions.length === 0 ? (
              <div className='flex h-24 flex-col items-center justify-center gap-1 text-sm text-muted-foreground'>
                <MonitorCheck className='size-5 opacity-40' />
                暂无会话（等待 Agent 上线…）
              </div>
            ) : (
              <ul className='divide-y'>
                {sessions.slice(0, 6).map((s) => (
                  <li key={s.id}>
                    <button
                      onClick={() => {
                        // 打开该会话的终端工作区标签并跳到会话页(无 /sessions/$id 详情路由)
                        openTab(sessionFnTab(s.id, s.hostname, 'terminal'))
                        void navigate({ to: '/sessions' })
                      }}
                      className='group flex w-full items-center gap-3 py-2.5 text-left transition-colors hover:bg-muted/50'
                    >
                      <span
                        className={cn(
                          'inline-block size-2 shrink-0 rounded-full',
                          s.online ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]' : 'bg-muted-foreground/40'
                        )}
                      />
                      <span className='w-24 truncate font-medium'>{s.hostname || `#${s.id}`}</span>
                      <Badge variant='outline' className='rounded-sm font-mono text-[10px]'>
                        {(s.privilege || '—').toUpperCase()}
                      </Badge>
                      <span className='hidden font-mono text-xs text-muted-foreground sm:inline'>{s.local_ip}</span>
                      <span className='hidden truncate text-xs text-muted-foreground md:inline'>{s.os}</span>
                      <span className='ms-auto text-xs text-muted-foreground'>{fmtRelative(s.last_seen)}</span>
                      <ChevronRight className='size-4 text-muted-foreground transition-transform group-hover:translate-x-0.5' />
                    </button>
                  </li>
                ))}
              </ul>
            )}
          </CardContent>
        </Card>
      </Main>
    </>
  )
}
