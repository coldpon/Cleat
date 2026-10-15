// 会话主控台页: CS/FlyC2 主界面逻辑 ——
// 上线列表(常驻上半区) + 底部工作区标签(终端/文件/进程), 多会话同屏切换。
// 行点击 = 打开/激活工作区标签(不跳路由)。
import { useMemo, useState, useEffect, useRef } from 'react'
import { useQuery } from '@tanstack/react-query'
import {
  flexRender,
  getCoreRowModel,
  getSortedRowModel,
  useReactTable,
  type ColumnDef,
  type SortingState,
} from '@tanstack/react-table'
import { ArrowUpDown, Monitor, RefreshCw, Search } from 'lucide-react'
import { listSessions, type SessionRow } from '@/lib/cleat-api'
import { sessionFnTab, useTabsStore } from '@/stores/tabs-store'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/skeleton'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { AppTabs } from '@/components/layout/app-tabs'
import { TabHost } from '@/components/layout/tab-host'
import { SessionContextMenu } from '@/components/session-context-menu'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search as GlobalSearch } from '@/components/search'

function fmtRelative(ts: number): string {
  const diff = Date.now() - ts
  if (diff < 10_000) return '刚刚'
  if (diff < 60_000) return `${Math.floor(diff / 1000)} 秒前`
  if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} 分钟前`
  if (diff < 86_400_000) return `${Math.floor(diff / 3_600_000)} 小时前`
  return `${Math.floor(diff / 86_400_000)} 天前`
}

function privilegeVariant(p: string): { className: string; label: string } {
  const s = p.toLowerCase()
  if (s.includes('system')) return { className: 'border-red-500/40 bg-red-500/10 text-red-400', label: 'SYSTEM' }
  if (s.includes('high') || s.includes('admin'))
    return { className: 'border-orange-500/40 bg-orange-500/10 text-orange-400', label: p.toUpperCase() }
  if (s.includes('medium')) return { className: 'border-sky-500/40 bg-sky-500/10 text-sky-400', label: 'MEDIUM' }
  if (s.includes('root')) return { className: 'border-rose-500/40 bg-rose-500/10 text-rose-400', label: 'ROOT' }
  return { className: 'border-muted-foreground/30 text-muted-foreground', label: p.toUpperCase() }
}

const columns: ColumnDef<SessionRow>[] = [
  {
    accessorKey: 'online',
    header: '状态',
    cell: ({ row }) => {
      const online = row.original.online
      return (
        <span className='flex items-center gap-2'>
          <span
            className={cn(
              'inline-block size-2 rounded-full',
              online ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.6)]' : 'bg-muted-foreground/40'
            )}
          />
          <span className={cn('text-xs', online ? 'text-emerald-500' : 'text-muted-foreground')}>
            {online ? '在线' : '离线'}
          </span>
        </span>
      )
    },
  },
  {
    accessorKey: 'id',
    header: ({ column }) => (
      <Button variant='ghost' size='sm' className='-ms-3 h-8' onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}>
        ID
        <ArrowUpDown className='ms-1 size-3.5' />
      </Button>
    ),
    cell: ({ row }) => <span className='font-mono text-xs text-muted-foreground'>#{row.original.id}</span>,
  },
  {
    accessorKey: 'hostname',
    header: ({ column }) => (
      <Button variant='ghost' size='sm' className='-ms-3 h-8' onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}>
        主机
        <ArrowUpDown className='ms-1 size-3.5' />
      </Button>
    ),
    cell: ({ row }) => <span className='font-medium'>{row.original.hostname || '—'}</span>,
  },
  {
    accessorKey: 'local_ip',
    header: '内网 IP',
    cell: ({ row }) => <span className='font-mono text-xs'>{row.original.local_ip || '—'}</span>,
  },
  {
    accessorKey: 'privilege',
    header: '权限',
    cell: ({ row }) => {
      const { className, label } = privilegeVariant(row.original.privilege || '')
      return (
        <Badge variant='outline' className={cn('rounded-sm font-mono text-[10px]', className)}>
          {label || '—'}
        </Badge>
      )
    },
  },
  {
    accessorKey: 'process_name',
    header: '进程',
    cell: ({ row }) => (
      <span className='text-sm'>
        {row.original.process_name || '—'}
        <span className='ms-1.5 font-mono text-xs text-muted-foreground'>({row.original.pid})</span>
      </span>
    ),
  },
  {
    accessorKey: 'os',
    header: '系统',
    cell: ({ row }) => <span className='text-xs text-muted-foreground'>{row.original.os || '—'}</span>,
  },
  {
    accessorKey: 'transport',
    header: '通道',
    cell: ({ row }) => (
      <Badge variant='secondary' className='rounded-sm font-mono text-[10px] uppercase'>
        {row.original.transport || '—'}
      </Badge>
    ),
  },
  {
    accessorKey: 'last_seen',
    header: ({ column }) => (
      <Button variant='ghost' size='sm' className='-ms-3 h-8' onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}>
        最后心跳
        <ArrowUpDown className='ms-1 size-3.5' />
      </Button>
    ),
    cell: ({ row }) => <span className='text-xs text-muted-foreground'>{fmtRelative(row.original.last_seen)}</span>,
  },
]

export function Sessions() {
  const [sorting, setSorting] = useState<SortingState>([{ id: 'id', desc: false }])
  const [filter, setFilter] = useState('')
  const [menu, setMenu] = useState<{ x: number; y: number; session: SessionRow } | null>(null)

  // 列表区高度(可拖拽, 像素; 持久化到 localStorage)
  const [listH, setListH] = useState(() => {
    const saved = Number(localStorage.getItem('cleat_list_h'))
    return saved >= 120 ? saved : 260
  })
  const dragRef = useRef<{ startY: number; startH: number } | null>(null)
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      if (!dragRef.current) return
      const next = dragRef.current.startH + (e.clientY - dragRef.current.startY)
      const max = window.innerHeight - 64 - 220
      setListH(Math.min(Math.max(next, 110), Math.max(120, max)))
    }
    const onUp = () => {
      if (!dragRef.current) return
      dragRef.current = null
      document.body.style.cursor = ''
      document.body.style.userSelect = ''
    }
    window.addEventListener('mousemove', onMove)
    window.addEventListener('mouseup', onUp)
    return () => {
      window.removeEventListener('mousemove', onMove)
      window.removeEventListener('mouseup', onUp)
    }
  }, [])
  // 拖拽结束持久化
  useEffect(() => {
    localStorage.setItem('cleat_list_h', String(listH))
  }, [listH])

  const { data, isLoading, isFetching, refetch } = useQuery({
    queryKey: ['sessions'],
    queryFn: listSessions,
    refetchInterval: 3000,
  })

  const openTab = useTabsStore((s) => s.openTab)
  const activeKey = useTabsStore((s) => s.activeKey)

  const rows = useMemo(() => {
    const list = data?.sessions ?? []
    const kw = filter.trim().toLowerCase()
    if (!kw) return list
    return list.filter(
      (s) =>
        s.hostname?.toLowerCase().includes(kw) ||
        s.local_ip?.includes(kw) ||
        s.process_name?.toLowerCase().includes(kw) ||
        String(s.id).includes(kw)
    )
  }, [data, filter])

  const table = useReactTable({
    data: rows,
    columns,
    state: { sorting },
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
  })

  const onlineCount = (data?.sessions ?? []).filter((s) => s.online).length

  return (
    <>
      <Header fixed>
        <GlobalSearch className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>

      <Main fluid className='flex h-[calc(100svh-4rem)] flex-col gap-0 overflow-hidden p-0'>
        {/* 上线列表区(常驻, 高度可拖拽) */}
        <section className='flex shrink-0 flex-col overflow-hidden' style={{ height: listH }}>
          {/* 紧凑工具条(寸土寸金: 一行搞定统计+过滤+刷新) */}
          <div className='flex h-9 shrink-0 items-center justify-between gap-2 border-b px-3'>
            <div className='flex items-center gap-1.5 text-xs text-muted-foreground'>
              <Monitor className='size-3.5' />
              <span className='font-medium text-foreground'>会话</span>
              <span className='text-emerald-500'>{onlineCount}</span>
              <span>/ {(data?.sessions ?? []).length}</span>
              <span className='ms-1 opacity-60'>· 点击行打开工作区 · 3s 刷新</span>
            </div>
            <div className='flex items-center gap-1.5'>
              <div className='relative'>
                <Search className='absolute start-2 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground' />
                <Input
                  placeholder='过滤主机 / IP / 进程…'
                  value={filter}
                  onChange={(e) => setFilter(e.target.value)}
                  className='h-7 w-44 ps-7 text-xs'
                />
              </div>
              <Button size='sm' variant='ghost' className='h-7 gap-1 px-2 text-xs' onClick={() => void refetch()}>
                <RefreshCw className={cn('size-3.5', isFetching && 'animate-spin')} />
                刷新
              </Button>
            </div>
          </div>
          <div className='min-h-0 flex-1 overflow-y-auto'>
            {isLoading ? (
              <div className='space-y-2 p-3'>
                {Array.from({ length: 4 }).map((_, i) => (
                  <Skeleton key={i} className='h-10 w-full' />
                ))}
              </div>
            ) : (
              <Table className='border'>
                <TableHeader>
                  {table.getHeaderGroups().map((hg) => (
                    <TableRow key={hg.id}>
                      {hg.headers.map((h) => (
                        <TableHead key={h.id}>
                          {h.isPlaceholder ? null : flexRender(h.column.columnDef.header, h.getContext())}
                        </TableHead>
                      ))}
                    </TableRow>
                  ))}
                </TableHeader>
                <TableBody>
                  {table.getRowModel().rows.length ? (
                    table.getRowModel().rows.map((row) => {
                      const isActive = activeKey === `s:${row.original.id}:terminal`
                      const host = row.original.hostname || `会话 #${row.original.id}`
                      return (
                        <TableRow
                          key={row.id}
                          className={cn('cursor-pointer', isActive && 'bg-accent hover:bg-accent')}
                          onClick={() => openTab(sessionFnTab(row.original.id, host, 'terminal'))}
                          onContextMenu={(e) => {
                            e.preventDefault()
                            setMenu({ x: e.clientX, y: e.clientY, session: row.original })
                          }}
                        >
                          {row.getVisibleCells().map((cell) => (
                            <TableCell key={cell.id}>
                              {flexRender(cell.column.columnDef.cell, cell.getContext())}
                            </TableCell>
                          ))}
                        </TableRow>
                      )
                    })
                  ) : (
                    <TableRow>
                      <TableCell colSpan={columns.length} className='h-32 text-center text-muted-foreground'>
                        暂无会话（等待 Agent 上线…）
                      </TableCell>
                    </TableRow>
                  )}
                </TableBody>
              </Table>
            )}
          </div>
        </section>

        {/* 拖拽分隔手柄: 上下拖动调整列表/工作区比例(CS/FlyC2 式) */}
        <div
          role='separator'
          aria-orientation='horizontal'
          aria-label='调整列表与工作区高度'
          onMouseDown={(e) => {
            e.preventDefault()
            dragRef.current = { startY: e.clientY, startH: listH }
            document.body.style.cursor = 'row-resize'
            document.body.style.userSelect = 'none'
          }}
          className='group relative h-1.5 shrink-0 cursor-row-resize transition-colors hover:bg-primary/20'
        >
          <div className='absolute inset-x-0 top-1/2 h-px -translate-y-1/2 bg-border group-hover:bg-primary/60' />
          <div className='absolute left-1/2 top-1/2 flex -translate-x-1/2 -translate-y-1/2 items-center rounded-full border bg-background px-1.5 py-[3px] opacity-0 shadow-sm transition-opacity group-hover:opacity-100'>
            <span className='block h-0.5 w-5 rounded bg-muted-foreground/50' />
          </div>
        </div>

        {/* 工作区标签栏(与列表同屏) */}
        <AppTabs />

        {/* 工作区内容 */}
        <section className='min-h-0 flex-1 overflow-hidden'>
          <TabHost />
        </section>
      </Main>

      {/* 会话行右键菜单 */}
      {menu && (
        <SessionContextMenu
          x={menu.x}
          y={menu.y}
          sessionId={menu.session.id}
          hostname={menu.session.hostname || `会话 #${menu.session.id}`}
          onClose={() => setMenu(null)}
        />
      )}
    </>
  )
}
