// 进程子页(真实对接 ): EnumProcess / KillProcess / SuspendProcess / ResumeProcess。
// 现为「主机管理」面板(host-mgmt-panel)的 进程 子标签(服务/网络/驱动/软件/窗口见该面板)。
// Agent 真实字段(ProcessEnumHandler): pid / parent_pid / name / thread_count / memory_bytes。
// 用户 / 架构 / CPU% 为预留展示位 —— 后端暂未提供, 缺数据显示 —,
// 不轮询: 每次枚举占一次 Agent 往返, 手动刷新为主。
// 性能: ①虚拟滚动(use-virtual-rows, 300+ 行只渲染可见行) ②每行不再挂 Radix DropdownMenu
//      (操作走右键/点骷髅弹出的单例菜单) ③过滤用 useDeferredValue(打字不卡表)。
import { useDeferredValue, useMemo, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ArrowUpDown, CirclePause, CirclePlay, Cpu, RefreshCw, Search, Skull } from 'lucide-react'
import { toast } from 'sonner'
import { listSessions, sendCommand } from '@/lib/cleat-api'
import { cn } from '@/lib/utils'
import { useVirtualRows } from '@/hooks/use-virtual-rows'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog'
import { Input } from '@/components/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'

/** Agent 真实字段 + 预留扩展位(user/arch/cpu 到位后自动显示) */
interface Proc {
  pid: number
  parent_pid: number
  name: string
  thread_count: number
  memory_bytes: number
  user?: string // 待后端: 进程属主(SID → 用户名)
  arch?: string // 待后端: x64 / x86
  cpu?: number // 待后端: CPU 占用 %(需两次快照差分)
}

type SortKey = 'pid' | 'name' | 'threads' | 'mem'

const ROW_H = 32 // 行高恒定(h-8, 单行 truncate), 虚拟滚动依赖此值
const COLS = 9

function formatMem(bytes: number): string {
  if (!bytes || bytes <= 0) return '—'
  const mb = bytes / 1024 / 1024
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`
  if (mb < 1) return `${(bytes / 1024).toFixed(0)} KB`
  return `${mb.toFixed(1)} MB`
}

function SortHeader({
  label,
  sortKey,
  sort,
  onSort,
  className,
}: {
  label: string
  sortKey: SortKey
  sort: { key: SortKey; dir: 'asc' | 'desc' }
  onSort: (k: SortKey) => void
  className?: string
}) {
  const active = sort.key === sortKey
  return (
    <TableHead className={className}>
      <button
        className='flex items-center gap-1 transition-colors hover:text-foreground'
        onClick={() => onSort(sortKey)}
      >
        {label}
        <ArrowUpDown className={cn('size-3', active && 'text-primary')} />
      </button>
    </TableHead>
  )
}

export function ProcessTab({ sessionId }: { sessionId: number }) {
  const [kw, setKw] = useState('')
  const deferredKw = useDeferredValue(kw)
  const [sort, setSort] = useState<{ key: SortKey; dir: 'asc' | 'desc' }>({ key: 'mem', dir: 'desc' })
  const [killTarget, setKillTarget] = useState<Proc | null>(null)
  // 右键/骷髅按钮弹出的单例操作菜单
  const [ctx, setCtx] = useState<{ x: number; y: number; proc: Proc } | null>(null)

  // 会话名(状态栏): 复用共享缓存
  const { data: sessData } = useQuery({
    queryKey: ['sessions'],
    queryFn: listSessions,
    refetchInterval: 3000,
  })
  const session = sessData?.sessions.find((s) => s.id === sessionId)

  // 进程列表: EnumProcess(手动刷新为主, 不轮询)
  const q = useQuery({
    queryKey: ['processes', sessionId],
    queryFn: async () => {
      const resp = await sendCommand<{ processes?: Record<string, unknown>[] }>(sessionId, 'EnumProcess')
      const list = resp.data?.processes ?? []
      return list.map((p) => ({
        pid: Number(p.pid ?? 0),
        parent_pid: Number(p.parent_pid ?? 0),
        name: String(p.name ?? `#${p.pid}`),
        thread_count: Number(p.thread_count ?? 0),
        memory_bytes: Number(p.memory_bytes ?? 0),
        user: typeof p.user === 'string' ? p.user : undefined,
        arch: typeof p.arch === 'string' ? p.arch : undefined,
        cpu: typeof p.cpu === 'number' ? p.cpu : undefined,
      })) as Proc[]
    },
    staleTime: 30_000,
    refetchOnWindowFocus: false,
    retry: false,
    // 离线守卫: 不对离线会话发枚举(否则傻等 310s 命令超时)
    enabled: !sessData || !!session?.online,
  })

  const procs = useMemo(() => {
    const k = deferredKw.trim().toLowerCase()
    let list = q.data ?? []
    if (k)
      list = list.filter(
        (p) =>
          p.name.toLowerCase().includes(k) ||
          String(p.pid).includes(k) ||
          String(p.parent_pid).includes(k) ||
          (p.user ?? '').toLowerCase().includes(k)
      )
    const dir = sort.dir === 'asc' ? 1 : -1
    return list.slice().sort((a, b) => {
      if (sort.key === 'name') return a.name.localeCompare(b.name, undefined, { numeric: true }) * dir
      if (sort.key === 'threads') return (a.thread_count - b.thread_count) * dir
      if (sort.key === 'mem') return (a.memory_bytes - b.memory_bytes) * dir
      return (a.pid - b.pid) * dir
    })
  }, [q.data, deferredKw, sort])

  // 虚拟滚动: 只渲染可见行(滚动容器 ref 由本组件持有, 传给钩子测算)
  const vRef = useRef<HTMLDivElement | null>(null)
  const v = useVirtualRows(procs.length, ROW_H, vRef)

  const onSort = (k: SortKey) =>
    setSort((prev) => (prev.key === k ? { key: k, dir: prev.dir === 'asc' ? 'desc' : 'asc' } : { key: k, dir: 'desc' }))

  const kill = async (p: Proc) => {
    try {
      await sendCommand(sessionId, 'KillProcess', { pid: p.pid })
      toast.success(`已终止 ${p.name}（pid ${p.pid}）`)
      void q.refetch()
    } catch (e) {
      toast.error(`终止失败: ${(e as Error).message}`)
    }
  }

  const suspendOrResume = async (p: Proc, resume: boolean) => {
    try {
      await sendCommand(sessionId, resume ? 'ResumeProcess' : 'SuspendProcess', { pid: p.pid })
      toast.success(`${resume ? '已恢复' : '已挂起'} ${p.name}（pid ${p.pid}）`)
    } catch (e) {
      toast.error(`操作失败: ${(e as Error).message}`)
    }
  }

  const isSystemUser = (u?: string) => u === 'SYSTEM' || u === 'root'
  const sysCount = procs.filter((p) => isSystemUser(p.user)).length
  const totalMemBytes = procs.reduce((s, p) => s + p.memory_bytes, 0)
  const visible = procs.slice(v.start, v.end)

  return (
    // select-none: 双击行不触发浏览器文本选择
    <div className='flex h-full min-h-0 select-none flex-col'>
      {/* 工具条 */}
      <div className='flex h-9 shrink-0 items-center gap-2 border-b px-3'>
        <div className='relative'>
          <Search className='absolute start-2 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground' />
          <Input
            placeholder='过滤进程名 / 用户 / PID…'
            value={kw}
            onChange={(e) => setKw(e.target.value)}
            className='h-7 w-56 ps-7 text-xs'
          />
        </div>
        <span className='text-xs text-muted-foreground'>
          {procs.length} 个进程{sysCount > 0 ? ` · 系统进程 ${sysCount} 个` : ''}
        </span>
        <Button
          size='sm'
          variant='ghost'
          className='ms-auto h-7 gap-1 px-2 text-xs'
          disabled={!!sessData && !session?.online}
          title={sessData && !session?.online ? '会话离线' : '重新枚举'}
          onClick={() => {
            if (sessData && !session?.online) {
              toast.error('会话离线，无法枚举进程')
              return
            }
            void q.refetch()
          }}
        >
          <RefreshCw className={cn('size-3.5', q.isFetching && 'animate-spin')} />
          刷新
        </Button>
      </div>

      {/* 进程表(虚拟滚动) */}
      <div ref={vRef} className='min-h-0 flex-1 overflow-auto'>
        <Table className='table-fixed'>
          <TableHeader className='sticky top-0 z-10 bg-background'>
            <TableRow className='hover:bg-transparent'>
              <SortHeader label='PID' sortKey='pid' sort={sort} onSort={onSort} className='w-20' />
              <TableHead className='w-20'>PPID</TableHead>
              <SortHeader label='名称' sortKey='name' sort={sort} onSort={onSort} />
              <TableHead className='w-28'>用户</TableHead>
              <SortHeader label='线程' sortKey='threads' sort={sort} onSort={onSort} className='w-16' />
              <TableHead className='w-16'>架构</TableHead>
              <TableHead className='w-20'>CPU</TableHead>
              <SortHeader label='内存' sortKey='mem' sort={sort} onSort={onSort} className='w-28' />
              <TableHead className='w-16 text-end'>操作</TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {v.padTop > 0 && (
              <TableRow aria-hidden className='hover:bg-transparent'>
                <TableCell colSpan={COLS} className='border-0 p-0' style={{ height: v.padTop }} />
              </TableRow>
            )}
            {q.isLoading ? (
              <TableRow>
                <TableCell colSpan={COLS} className='h-32 text-center text-sm text-muted-foreground'>
                  枚举中…（等待 Agent 往返）
                </TableCell>
              </TableRow>
            ) : q.isError ? (
              <TableRow>
                <TableCell colSpan={COLS} className='h-32 text-center text-sm text-muted-foreground'>
                  枚举失败：{(q.error as Error).message}
                  <Button size='sm' variant='outline' className='ms-3 h-7 text-xs' onClick={() => void q.refetch()}>
                    重试
                  </Button>
                </TableCell>
              </TableRow>
            ) : procs.length === 0 ? (
              <TableRow>
                <TableCell colSpan={COLS} className='h-32 text-center text-sm text-muted-foreground'>
                  无匹配进程
                </TableCell>
              </TableRow>
            ) : (
              visible.map((p) => {
                const isSystem = isSystemUser(p.user)
                return (
                  <TableRow
                    key={p.pid}
                    className='h-8 cursor-default text-xs'
                    onContextMenu={(e) => {
                      e.preventDefault()
                      setCtx({ x: e.clientX, y: e.clientY, proc: p })
                    }}
                  >
                    <TableCell className='py-1 font-mono'>{p.pid}</TableCell>
                    <TableCell className='py-1 font-mono text-muted-foreground'>{p.parent_pid || '—'}</TableCell>
                    <TableCell className='py-1'>
                      <span className='flex items-center gap-1.5'>
                        <Cpu className={cn('size-3 shrink-0', isSystem ? 'text-muted-foreground' : 'text-sky-400')} />
                        <span className='truncate font-mono' title={p.name}>
                          {p.name}
                        </span>
                      </span>
                    </TableCell>
                    <TableCell className='py-1'>
                      {p.user ? (
                        isSystem ? (
                          <Badge
                            variant='outline'
                            className='rounded-sm border-red-500/40 bg-red-500/10 font-mono text-[10px] text-red-400'
                          >
                            {p.user.toUpperCase()}
                          </Badge>
                        ) : (
                          <span className='truncate font-mono text-muted-foreground' title={p.user}>
                            {p.user}
                          </span>
                        )
                      ) : (
                        <span className='text-muted-foreground/50'>—</span>
                      )}
                    </TableCell>
                    <TableCell className='py-1 font-mono text-muted-foreground'>{p.thread_count || '—'}</TableCell>
                    <TableCell className='py-1 font-mono text-muted-foreground'>
                      {p.arch ?? <span className='text-muted-foreground/50'>—</span>}
                    </TableCell>
                    <TableCell className='py-1 font-mono text-muted-foreground'>
                      {typeof p.cpu === 'number' ? `${p.cpu.toFixed(1)}%` : <span className='text-muted-foreground/50'>—</span>}
                    </TableCell>
                    <TableCell className='py-1 font-mono text-muted-foreground'>{formatMem(p.memory_bytes)}</TableCell>
                    <TableCell className='py-1 text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 w-6 p-0 text-muted-foreground hover:text-red-400'
                        title='进程操作（右键行也可）'
                        onClick={(e) => {
                          e.stopPropagation()
                          setCtx({ x: e.clientX, y: e.clientY, proc: p })
                        }}
                      >
                        <Skull className='size-3.5' />
                      </Button>
                    </TableCell>
                  </TableRow>
                )
              })
            )}
            {v.padBottom > 0 && (
              <TableRow aria-hidden className='hover:bg-transparent'>
                <TableCell colSpan={COLS} className='border-0 p-0' style={{ height: v.padBottom }} />
              </TableRow>
            )}
          </TableBody>
        </Table>
      </div>

      {/* 底部状态栏 */}
      <div className='flex h-7 shrink-0 items-center justify-between border-t px-3 text-xs text-muted-foreground'>
        <span className='truncate font-mono'>{session?.hostname || `会话 #${sessionId}`}</span>
        <span>
          {procs.length} 个进程 · 内存合计 {formatMem(totalMemBytes)}
        </span>
      </div>

      {/* 右键/骷髅操作菜单(单例, 避免每行挂一个 Radix 菜单) */}
      {ctx && (
        <>
          <div className='fixed inset-0 z-50' onClick={() => setCtx(null)} onContextMenu={(e) => { e.preventDefault(); setCtx(null) }} />
          <div
            className='fixed z-50 min-w-36 rounded-md border bg-popover p-1 text-popover-foreground shadow-md'
            style={{
              left: Math.min(ctx.x, window.innerWidth - 170),
              top: Math.min(ctx.y, window.innerHeight - 140),
            }}
          >
            <button
              className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm text-red-500 transition-colors hover:bg-accent'
              onClick={() => {
                setKillTarget(ctx.proc)
                setCtx(null)
              }}
            >
              <Skull className='size-3.5' />
              终止进程
            </button>
            <button
              className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
              onClick={() => {
                void suspendOrResume(ctx.proc, false)
                setCtx(null)
              }}
            >
              <CirclePause className='size-3.5 text-muted-foreground' />
              挂起
            </button>
            <button
              className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
              onClick={() => {
                void suspendOrResume(ctx.proc, true)
                setCtx(null)
              }}
            >
              <CirclePlay className='size-3.5 text-muted-foreground' />
              恢复
            </button>
          </div>
        </>
      )}

      {/* 终止确认 */}
      <AlertDialog open={killTarget !== null} onOpenChange={(o) => !o && setKillTarget(null)}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>终止进程「{killTarget?.name}」？</AlertDialogTitle>
            <AlertDialogDescription>
              将向目标主机发送 KillProcess（pid {killTarget?.pid}），该进程会被强制结束，未保存的数据将丢失。
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>取消</AlertDialogCancel>
            <AlertDialogAction
              onClick={() => {
                if (killTarget) void kill(killTarget)
                setKillTarget(null)
              }}
            >
              终止
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  )
}
