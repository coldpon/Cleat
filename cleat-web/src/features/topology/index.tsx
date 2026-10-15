// 拓扑图页(真实对接 ): 转发模块成果可视化(移植 cleat-web TopologyPage/topologyGraph)。
// 数据源: 会话 + 监听器 + SOCKS 出口(/api/pivots) + 反向端口转发(/api/rportfwds),
// 前端组装为 [TeamServer → Agent → 隧道端点] 三列拓扑, 自绘 SVG(不引图库依赖)。
// 只读展示; 隧道的新建/删除在「隧道」页操作。
import { useMemo, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Focus, Loader2, RefreshCw, ZoomIn, ZoomOut } from 'lucide-react'
import { listSessions, type SessionRow } from '@/lib/cleat-api'
import { listListeners, listPivots, listRPortFwds } from '@/lib/cleat-rest'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Switch } from '@/components/ui/switch'
import { Sheet, SheetContent, SheetHeader, SheetTitle } from '@/components/ui/sheet'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'
import { sessionFnTab, useTabsStore } from '@/stores/tabs-store'
import {
  TOPO,
  buildTopologyGraph,
  fitText,
  tunnelCaption,
  tunnelDetails,
  tunnelTitle,
  type TopoEdge,
  type TopoNode,
  type TopoTunnel,
} from './topology-graph'

// 隧道类型配色(亮/暗主题下均可读)
const KIND_COLOR: Record<TopoTunnel['kind'], string> = {
  socks: '#06b6d4',
  chain: '#f59e0b',
  rportfwd: '#8b5cf6',
}
const TUNNEL_TAG_WIDTH: Record<TopoTunnel['kind'], number> = { socks: 56, chain: 40, rportfwd: 40 }
const GREEN = '#10b981'
const GREY = '#94a3b8'

function fmtTime(ts: number): string {
  const d = new Date(ts)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

export function Topology() {
  const navigate = useNavigate()
  const canWrite = useCanWrite()
  const openTab = useTabsStore((s) => s.openTab)

  const [autoRefresh, setAutoRefresh] = useState(true)
  const [scale, setScale] = useState(1)
  const [hovered, setHovered] = useState<string | null>(null)
  const [selectedId, setSelectedId] = useState<string | null>(null)

  const poll = { refetchInterval: autoRefresh ? 5000 : (false as const) }
  const sessionsQ = useQuery({ queryKey: ['sessions'], queryFn: listSessions, ...poll })
  const listenersQ = useQuery({ queryKey: ['listeners'], queryFn: listListeners, ...poll })
  const pivotsQ = useQuery({ queryKey: ['pivots'], queryFn: listPivots, ...poll })
  const rportfwdsQ = useQuery({ queryKey: ['rportfwds'], queryFn: listRPortFwds, ...poll })

  const sessions = useMemo(() => sessionsQ.data?.sessions ?? [], [sessionsQ.data])
  const listeners = useMemo(() => listenersQ.data?.listeners ?? [], [listenersQ.data])
  const pivots = useMemo(() => pivotsQ.data?.pivots ?? [], [pivotsQ.data])
  const rportfwds = useMemo(() => rportfwdsQ.data?.rportfwds ?? [], [rportfwdsQ.data])

  const loading = sessionsQ.isLoading || listenersQ.isLoading || pivotsQ.isLoading || rportfwdsQ.isLoading
  const errors = [sessionsQ.error, listenersQ.error, pivotsQ.error, rportfwdsQ.error].filter(Boolean) as Error[]
  const updatedAt = Math.max(
    sessionsQ.dataUpdatedAt ?? 0,
    listenersQ.dataUpdatedAt ?? 0,
    pivotsQ.dataUpdatedAt ?? 0,
    rportfwdsQ.dataUpdatedAt ?? 0
  )

  const graph = useMemo(
    () => buildTopologyGraph({ sessions, listeners, pivots, rportfwds }),
    [sessions, listeners, pivots, rportfwds]
  )

  // 悬停/选中聚焦: 只保留相邻元素, 其余淡化
  const focusId = hovered ?? selectedId
  const related = useMemo(() => {
    const set = new Set<string>()
    if (!focusId) return set
    set.add(focusId)
    for (const e of graph.edges) {
      if (e.source === focusId) set.add(e.target)
      if (e.target === focusId) set.add(e.source)
    }
    return set
  }, [focusId, graph.edges])
  const dim = (id: string) => (focusId && !related.has(id) ? 0.28 : 1)
  const edgeDim = (e: TopoEdge) => (focusId && e.source !== focusId && e.target !== focusId ? 0.15 : 1)

  const selected = graph.nodes.find((n) => n.id === selectedId) ?? null

  const stats = {
    sessions: sessions.length,
    online: sessions.filter((s) => s.online).length,
    socks: pivots.filter((p) => !p.entry_host || !p.entry_port).length,
    chain: pivots.filter((p) => !!p.entry_host && !!p.entry_port).length,
    rportfwd: rportfwds.length,
  }
  const empty = !loading && !sessions.length && !pivots.length && !rportfwds.length

  const edgeColor = (e: TopoEdge) => (e.kind === 'c2' ? (e.online ? GREEN : GREY) : KIND_COLOR[e.kind])
  const nodeStroke = (n: TopoNode) =>
    n.id === selectedId ? 'stroke-primary' : n.id === hovered ? 'stroke-primary/60' : 'stroke-border'

  const enterWorkspace = (s: SessionRow) => {
    openTab(sessionFnTab(s.id, s.hostname || `#${s.id}`, 'terminal'))
    void navigate({ to: '/sessions' })
  }

  // ============ SVG 片段 ============

  const renderServerNode = (n: TopoNode) => {
    const chips = (n.listeners ?? []).slice(0, TOPO.maxChips)
    const more = (n.listeners?.length ?? 0) - chips.length
    return (
      <g
        key={n.id}
        opacity={dim(n.id)}
        style={{ cursor: 'pointer' }}
        onClick={() => setSelectedId(n.id)}
        onMouseEnter={() => setHovered(n.id)}
        onMouseLeave={() => setHovered(null)}
      >
        <rect x={n.x} y={n.y} width={n.w} height={n.h} rx={10} className={cn('fill-card', nodeStroke(n))} strokeWidth={1.5} />
        <rect x={n.x} y={n.y} width={n.w} height={4} rx={2} className='fill-primary' />
        <text x={n.x + 14} y={n.y + 30} fontSize={14} fontWeight={700} className='fill-foreground'>
          TeamServer
        </text>
        <text x={n.x + 14} y={n.y + 50} fontSize={11} className='fill-muted-foreground'>
          监听 {listeners.length} · 隧道 {stats.socks + stats.chain + stats.rportfwd}
        </text>
        {chips.map((l, i) => (
          <g key={l.address}>
            <rect x={n.x + 12} y={n.y + 60 + i * TOPO.chipH} width={n.w - 24} height={TOPO.chipH - 6} rx={5} className='fill-muted' />
            <text x={n.x + 20} y={n.y + 60 + i * TOPO.chipH + 13} fontSize={10.5} className='fill-muted-foreground'>
              {l.status === 'running' ? '● ' : '○ '}
              {l.protocol} {l.address}
            </text>
          </g>
        ))}
        {more > 0 && (
          <text x={n.x + 14} y={n.y + 60 + chips.length * TOPO.chipH + 13} fontSize={10.5} className='fill-muted-foreground/70'>
            +{more} 个监听器…
          </text>
        )}
      </g>
    )
  }

  const renderAgentNode = (n: TopoNode) => {
    const s = n.session!
    return (
      <g
        key={n.id}
        opacity={dim(n.id)}
        style={{ cursor: 'pointer' }}
        onClick={() => setSelectedId(n.id)}
        onMouseEnter={() => setHovered(n.id)}
        onMouseLeave={() => setHovered(null)}
      >
        <rect x={n.x} y={n.y} width={n.w} height={n.h} rx={10} className={cn('fill-card', nodeStroke(n))} strokeWidth={1.5} />
        <rect x={n.x} y={n.y + 10} width={4} height={n.h - 20} rx={2} fill={s.online ? GREEN : GREY} opacity={s.online ? 1 : 0.5} />
        <circle cx={n.x + 20} cy={n.y + 24} r={4.5} fill={s.online ? GREEN : GREY} opacity={s.online ? 1 : 0.5} />
        <text x={n.x + 32} y={n.y + 29} fontSize={13} fontWeight={700} className='fill-foreground'>
          {fitText(`#${s.id} ${s.hostname || '(未知主机)'}`, 18)}
        </text>
        <text x={n.x + 14} y={n.y + 51} fontSize={11} className='fill-muted-foreground'>
          {s.remote_ip || '—'} · {(s.transport || '未知').toUpperCase()}
        </text>
        <text x={n.x + 14} y={n.y + 71} fontSize={10.5} className='fill-muted-foreground/70'>
          {fitText(`${s.os || '?'} · PID ${s.pid} ${s.process_name || ''}`, 32)}
        </text>
      </g>
    )
  }

  const renderTunnelNode = (n: TopoNode) => {
    const t = n.tunnel!
    const color = KIND_COLOR[t.kind]
    const tagW = TUNNEL_TAG_WIDTH[t.kind]
    return (
      <g
        key={n.id}
        opacity={dim(n.id)}
        style={{ cursor: 'pointer' }}
        onClick={() => setSelectedId(n.id)}
        onMouseEnter={() => setHovered(n.id)}
        onMouseLeave={() => setHovered(null)}
      >
        <rect x={n.x} y={n.y} width={n.w} height={n.h} rx={8} className={cn('fill-card', nodeStroke(n))} strokeWidth={1.5} />
        <rect x={n.x + 10} y={n.y + 9} width={tagW} height={18} rx={4} fill={color} fillOpacity={0.16} />
        <text x={n.x + 10 + tagW / 2} y={n.y + 22} fontSize={10} fontWeight={700} fill={color} textAnchor='middle'>
          {t.kind === 'socks' ? 'SOCKS5' : t.kind === 'chain' ? '多跳' : '转发'}
        </text>
        <text x={n.x + 10 + tagW + 8} y={n.y + 22} fontSize={12} fontWeight={600} className='fill-foreground'>
          {fitText(tunnelTitle(t), 20)}
        </text>
        <text x={n.x + 10} y={n.y + 45} fontSize={10.5} className='fill-muted-foreground/70'>
          {fitText(tunnelCaption(t), 28)}
        </text>
      </g>
    )
  }

  const renderEdge = (e: TopoEdge) => {
    const color = edgeColor(e)
    const dashed = e.kind !== 'c2' || !e.online
    return (
      <g key={e.id} opacity={edgeDim(e)}>
        <path
          d={e.d}
          fill='none'
          stroke={color}
          strokeWidth={1.6}
          strokeDasharray={dashed ? '6 4' : undefined}
          markerEnd={`url(#arrow-${e.kind}-${e.online ? 'on' : 'off'})`}
        />
        {e.kind === 'c2' && e.label && (
          <>
            <rect
              x={e.mx - (e.label.length * 6.6 + 12) / 2}
              y={e.my - 9}
              width={e.label.length * 6.6 + 12}
              height={18}
              rx={9}
              className='fill-card stroke-border'
            />
            <text x={e.mx} y={e.my + 3.5} fontSize={10} textAnchor='middle' className='fill-muted-foreground'>
              {e.label}
            </text>
          </>
        )}
      </g>
    )
  }

  // ============ 详情面板(Sheet) ============

  const KV = ({ label, children }: { label: string; children: React.ReactNode }) => (
    <div className='flex items-start justify-between gap-3 border-b px-3 py-2 text-xs last:border-b-0'>
      <span className='shrink-0 text-muted-foreground'>{label}</span>
      <span className='break-all text-right'>{children}</span>
    </div>
  )

  const renderDrawer = () => {
    if (!selected) return null
    if (selected.kind === 'agent') {
      const s = selected.session!
      return (
        <>
          <div className='overflow-hidden rounded-md border'>
            <KV label='会话 ID'>#{s.id}</KV>
            <KV label='主机名'>{s.hostname || '—'}</KV>
            <KV label='远端 IP'>{s.remote_ip || '—'}</KV>
            <KV label='系统'>{s.os || '—'}</KV>
            <KV label='进程'>
              {s.process_name || '—'} (PID {s.pid})
            </KV>
            <KV label='权限'>{s.privilege || '—'}</KV>
            <KV label='通道'>{(s.transport || '未知').toUpperCase()}</KV>
            <KV label='最近活跃'>{s.last_seen ? new Date(s.last_seen).toLocaleString() : '—'}</KV>
            <KV label='状态'>
              <span className={s.online ? 'text-emerald-500' : 'text-muted-foreground'}>{s.online ? '在线' : '离线'}</span>
            </KV>
          </div>
          <Button className='mt-4 w-full' onClick={() => enterWorkspace(s)}>
            进入会话工作台
          </Button>
        </>
      )
    }
    if (selected.kind === 'tunnel') {
      const t = selected.tunnel!
      const agent = sessions.find((x) => x.id === t.sessionId)
      return (
        <>
          <div className='overflow-hidden rounded-md border'>
            {tunnelDetails(t).map(([k, v]) => (
              <KV key={k} label={k}>
                {v}
              </KV>
            ))}
          </div>
          {agent && (
            <Button variant='outline' className='mt-4 w-full' onClick={() => setSelectedId(`agent-${t.sessionId}`)}>
              查看出会话 #{t.sessionId} {agent.hostname}
            </Button>
          )}
        </>
      )
    }
    return (
      <>
        <div className='overflow-hidden rounded-md border'>
          <KV label='监听器'>{listeners.length} 个</KV>
          <KV label='隧道端点'>{stats.socks + stats.chain + stats.rportfwd} 个</KV>
          {listeners.map((l) => (
            <KV key={l.address} label={`${l.protocol.toUpperCase()} ${l.address}`}>
              <span className={l.status === 'running' ? 'text-emerald-500' : 'text-muted-foreground'}>
                {l.status === 'running' ? '运行中' : '已停止'}
              </span>
            </KV>
          ))}
        </div>
        {canWrite && (
          <Button variant='outline' className='mt-4 w-full' onClick={() => void navigate({ to: '/pivots' })}>
            前往隧道管理
          </Button>
        )}
      </>
    )
  }

  // ============ 页面 ============

  return (
    <>
      <Header fixed>
        <Search className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main>
        <div className='mb-3 flex flex-wrap items-center justify-between gap-2'>
          <div>
            <h2 className='text-2xl font-bold tracking-tight'>拓扑图</h2>
            <p className='text-muted-foreground'>转发链路总览：TeamServer → Beacon 会话 → 隧道端点（SOCKS 出口 / 多跳链 / 反向端口转发）</p>
          </div>
          <div className='flex flex-wrap items-center gap-1.5'>
            <Badge variant='secondary' className='rounded-sm'>会话 {stats.sessions}</Badge>
            <Badge
              variant='outline'
              className={cn(
                'rounded-sm',
                stats.online > 0 ? 'border-emerald-500/40 bg-emerald-500/10 text-emerald-600 dark:text-emerald-400' : 'text-muted-foreground'
              )}
            >
              在线 {stats.online}
            </Badge>
            <Badge variant='outline' className='rounded-sm border-cyan-500/40 bg-cyan-500/10 text-cyan-600 dark:text-cyan-400'>
              SOCKS {stats.socks}
            </Badge>
            <Badge variant='outline' className='rounded-sm border-amber-500/40 bg-amber-500/10 text-amber-600 dark:text-amber-400'>
              多跳 {stats.chain}
            </Badge>
            <Badge variant='outline' className='rounded-sm border-violet-500/40 bg-violet-500/10 text-violet-600 dark:text-violet-400'>
              转发 {stats.rportfwd}
            </Badge>
          </div>
        </div>

        <div className='mb-3 flex flex-wrap items-center justify-between gap-3'>
          <span className='text-xs text-muted-foreground'>
            <span style={{ color: GREEN }}>━</span> C2 链路 · <span style={{ color: GREY }}>┄</span> 离线 ·
            <span style={{ color: KIND_COLOR.socks }}> ┄</span> SOCKS 出口 ·
            <span style={{ color: KIND_COLOR.chain }}> ┄</span> 多跳链 ·
            <span style={{ color: KIND_COLOR.rportfwd }}> ┄</span> 反向转发
          </span>
          <div className='flex flex-wrap items-center gap-2'>
            {updatedAt > 0 && <span className='text-xs text-muted-foreground'>更新于 {fmtTime(updatedAt)}</span>}
            <label className='flex items-center gap-1.5 text-xs text-muted-foreground'>
              自动刷新
              <Switch checked={autoRefresh} onCheckedChange={setAutoRefresh} />
            </label>
            <Button
              size='sm'
              variant='outline'
              className='h-7 gap-1 text-xs'
              onClick={() =>
                void Promise.all([sessionsQ.refetch(), listenersQ.refetch(), pivotsQ.refetch(), rportfwdsQ.refetch()])
              }
            >
              <RefreshCw className={cn('size-3.5', (sessionsQ.isFetching || pivotsQ.isFetching) && 'animate-spin')} />
              刷新
            </Button>
            <div className='flex items-center gap-0.5 rounded-md border p-0.5'>
              <Button size='sm' variant='ghost' className='size-6 p-0' title='缩小' onClick={() => setScale((v) => Math.max(0.5, +(v - 0.2).toFixed(1)))}>
                <ZoomOut className='size-3.5' />
              </Button>
              <Button size='sm' variant='ghost' className='size-6 p-0' title='复位' onClick={() => setScale(1)}>
                <Focus className='size-3.5' />
              </Button>
              <Button size='sm' variant='ghost' className='size-6 p-0' title='放大' onClick={() => setScale((v) => Math.min(2, +(v + 0.2).toFixed(1)))}>
                <ZoomIn className='size-3.5' />
              </Button>
            </div>
          </div>
        </div>

        {errors.length > 0 && (
          <div className='mb-3 rounded-md border border-amber-500/40 bg-amber-500/5 px-3 py-2 text-xs text-amber-700 dark:text-amber-400'>
            部分数据加载失败，拓扑可能不完整：{errors.map((e) => e.message).join('；')}
          </div>
        )}

        {loading ? (
          <div className='flex h-96 items-center justify-center gap-2 rounded-lg border text-sm text-muted-foreground'>
            <Loader2 className='size-4 animate-spin' />
            加载中…
          </div>
        ) : empty ? (
          <div className='flex h-96 flex-col items-center justify-center gap-3 rounded-lg border border-dashed text-sm text-muted-foreground'>
            <p>暂无会话与隧道 —— 上线第一个 Agent 并建立转发后，链路会出现在这里</p>
            {canWrite && (
              <Button size='sm' className='h-7 text-xs' onClick={() => void navigate({ to: '/payloads' })}>
                生成载荷
              </Button>
            )}
          </div>
        ) : (
          <div className='overflow-auto rounded-lg border bg-card' style={{ maxHeight: '72vh' }}>
            <svg
              width={graph.width * scale}
              height={graph.height * scale}
              viewBox={`0 0 ${graph.width} ${graph.height}`}
              style={{ display: 'block' }}
            >
              <defs>
                {(
                  [
                    ['c2-on', GREEN],
                    ['c2-off', GREY],
                    ['socks-on', KIND_COLOR.socks],
                    ['socks-off', KIND_COLOR.socks],
                    ['chain-on', KIND_COLOR.chain],
                    ['chain-off', KIND_COLOR.chain],
                    ['rportfwd-on', KIND_COLOR.rportfwd],
                    ['rportfwd-off', KIND_COLOR.rportfwd],
                  ] as Array<[string, string]>
                ).map(([id, color]) => (
                  <marker key={id} id={`arrow-${id}`} markerWidth={8} markerHeight={8} refX={7} refY={4} orient='auto'>
                    <path d='M 0 0 L 8 4 L 0 8 z' fill={color} />
                  </marker>
                ))}
              </defs>
              {graph.edges.map(renderEdge)}
              {graph.nodes.map((n) => (n.kind === 'server' ? renderServerNode(n) : n.kind === 'agent' ? renderAgentNode(n) : renderTunnelNode(n)))}
            </svg>
          </div>
        )}

        <Sheet open={!!selected} onOpenChange={(o) => !o && setSelectedId(null)}>
          <SheetContent className='w-[400px] overflow-y-auto sm:max-w-[400px]'>
            <SheetHeader>
              <SheetTitle>
                {selected
                  ? selected.kind === 'agent'
                    ? `会话 #${selected.session?.id}`
                    : selected.kind === 'tunnel'
                      ? `隧道端点 — ${selected.tunnel?.kind === 'socks' ? 'SOCKS5 出口' : selected.tunnel?.kind === 'chain' ? '多跳链' : '反向端口转发'}`
                      : 'TeamServer'
                  : ''}
              </SheetTitle>
            </SheetHeader>
            <div className='px-4 pb-4'>{renderDrawer()}</div>
          </SheetContent>
        </Sheet>
      </Main>
    </>
  )
}
