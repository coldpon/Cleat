// 主机管理面板(真实对接 ): 对齐老 HostManagementPage —— 进程/服务/网络/驱动/软件/窗口 六子标签。
// 命令面(Agent CmdDef): EnumProcess/KillProcess/SuspendProcess/ResumeProcess ·
//   EnumService/StartService/StopService · EnumNetwork · EnumDriver/StartDriver/StopDriver ·
//   EnumSoftware · EnumWindow/CloseWindow/SetTopMostWindow/RemoveTopMostWindow。
// 平台裁剪: 服务/驱动/软件/窗口 为 Windows 能力, Linux 会话自动隐藏(与老版同款判据)。
// 数据加载: 子标签激活才发枚举(每次枚举占一次 Agent 往返, 不轮询, 手动刷新; 60s 内切换不重复请求)。
import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { RefreshCw } from 'lucide-react'
import { toast } from 'sonner'
import { listSessions, sendCommand } from '@/lib/cleat-api'
import { isLinuxOs } from '@/lib/platform'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { ProcessTab } from './process-panel'

// ============ 数据类型(与 Agent 返回体对齐) ============

interface ServiceItem {
  name: string
  display_name: string
  state: string
  start_type: string
  process_id: number
  binary_path: string
}
interface NetworkItem {
  local_addr: string
  local_port: number
  remote_addr?: string
  remote_port?: number
  state: string
  pid: number
  process_name: string
}
interface DriverItem {
  name: string
  display_name: string
  state: string
  start_type: string
  is_kernel_driver: boolean
  binary_path: string
}
interface SoftwareItem {
  display_name: string
  publisher: string
  version: string
  install_location: string
  install_date: string
}
interface WindowItem {
  handle: number
  title: string
  class_name: string
  is_visible: boolean
  is_topmost: boolean
  process_id: number
  process_name: string
}

// ============ 公共小件 ============

/** 枚举类查询(激活才发; 手动刷新为主) */
function useEnumList<T>(sessionId: number, cmd: string, key: string, online: boolean) {
  return useQuery({
    queryKey: ['host-enum', cmd, key, sessionId],
    queryFn: async () => {
      const resp = await sendCommand<Record<string, unknown>>(sessionId, cmd)
      const d = (resp.data ?? resp) as Record<string, unknown>
      const list = d[key]
      return Array.isArray(list) ? (list as T[]) : []
    },
    enabled: online,
    staleTime: 60_000,
    refetchOnWindowFocus: false,
    retry: false,
  })
}

function stateBadge(state: unknown) {
  const s = String(state || '').toLowerCase()
  let cls = 'border-amber-500/40 bg-amber-500/10 text-amber-600 dark:text-amber-400'
  if (s.includes('run') || s.includes('active') || s.includes('listen') || s.includes('establish')) {
    cls = 'border-emerald-500/40 bg-emerald-500/10 text-emerald-600 dark:text-emerald-400'
  } else if (s.includes('stop') || s.includes('dead') || s.includes('disable')) {
    cls = 'border-muted-foreground/30 text-muted-foreground'
  } else if (s.includes('error')) {
    cls = 'border-red-500/40 bg-red-500/10 text-red-600 dark:text-red-400'
  }
  return (
    <Badge variant='outline' className={cn('rounded-sm font-mono text-[10px]', cls)}>
      {String(state || '—')}
    </Badge>
  )
}

function boolBadge(v: boolean, yes = '是', no = '否') {
  return (
    <Badge
      variant='outline'
      className={cn('rounded-sm text-[10px]', v ? 'border-emerald-500/40 text-emerald-600 dark:text-emerald-400' : 'border-muted-foreground/30 text-muted-foreground')}
    >
      {v ? yes : no}
    </Badge>
  )
}

/** 子标签工具条: 统计 + 刷新 */
function TabToolbar({
  label,
  count,
  online,
  isFetching,
  onRefresh,
}: {
  label: string
  count?: number
  online: boolean
  isFetching: boolean
  onRefresh: () => void
}) {
  return (
    <div className='flex h-8 shrink-0 items-center gap-2'>
      <span className='text-xs text-muted-foreground'>
        {label}
        {count != null ? ` · ${count} 项` : ''}
      </span>
      <Button
        size='sm'
        variant='ghost'
        className='ms-auto h-7 gap-1 px-2 text-xs'
        disabled={!online || isFetching}
        title={!online ? '会话离线' : '重新枚举'}
        onClick={onRefresh}
      >
        <RefreshCw className={cn('size-3.5', isFetching && 'animate-spin')} />
        刷新
      </Button>
    </div>
  )
}

/** 表格空/载/错态行 */
function StateRow({ colSpan, text, retry }: { colSpan: number; text: string; retry?: () => void }) {
  return (
    <TableRow className='hover:bg-transparent'>
      <TableCell colSpan={colSpan} className='h-28 text-center text-sm text-muted-foreground'>
        {text}
        {retry && (
          <Button size='sm' variant='outline' className='ms-3 h-7 text-xs' onClick={retry}>
            重试
          </Button>
        )}
      </TableCell>
    </TableRow>
  )
}

/** 表头行(统一 hover 样式) */
function HeadRow({ children }: { children: React.ReactNode }) {
  return (
    <TableRow className='hover:bg-transparent'>
      {children}
    </TableRow>
  )
}

// ============ 主面板 ============

export function HostMgmtPanel({ sessionId }: { sessionId: number }) {
  const { data: sessData } = useQuery({ queryKey: ['sessions'], queryFn: listSessions, refetchInterval: 3000 })
  const session = sessData?.sessions.find((s) => s.id === sessionId)
  const online = !!session?.online
  const linux = isLinuxOs(session?.os || '')
  const [tab, setTab] = useState('process')

  return (
    <Tabs value={tab} onValueChange={setTab} className='flex h-full min-h-0 flex-col gap-0'>
      <div className='flex items-center gap-2 px-2 pt-1.5'>
        <TabsList>
          <TabsTrigger value='process'>进程</TabsTrigger>
          {!linux && <TabsTrigger value='service'>服务</TabsTrigger>}
          <TabsTrigger value='network'>网络</TabsTrigger>
          {!linux && <TabsTrigger value='driver'>驱动</TabsTrigger>}
          {!linux && <TabsTrigger value='software'>软件</TabsTrigger>}
          {!linux && <TabsTrigger value='window'>窗口</TabsTrigger>}
        </TabsList>
        <span className='ms-auto text-xs text-muted-foreground'>
          {linux ? 'Linux 会话：进程 / 网络' : online ? '' : '会话离线'}
        </span>
      </div>

      {/* 进程: 保活(过滤/虚拟滚动状态不丢) */}
      <TabsContent value='process' forceMount className='flex min-h-0 flex-1 flex-col data-[state=inactive]:hidden'>
        <ProcessTab sessionId={sessionId} />
      </TabsContent>
      <TabsContent value='service' className='flex min-h-0 flex-1 flex-col'>
        <ServiceTab sessionId={sessionId} online={online} />
      </TabsContent>
      <TabsContent value='network' className='flex min-h-0 flex-1 flex-col'>
        <NetworkTab sessionId={sessionId} online={online} />
      </TabsContent>
      <TabsContent value='driver' className='flex min-h-0 flex-1 flex-col'>
        <DriverTab sessionId={sessionId} online={online} />
      </TabsContent>
      <TabsContent value='software' className='flex min-h-0 flex-1 flex-col'>
        <SoftwareTab sessionId={sessionId} online={online} />
      </TabsContent>
      <TabsContent value='window' className='flex min-h-0 flex-1 flex-col'>
        <WindowTab sessionId={sessionId} online={online} />
      </TabsContent>
    </Tabs>
  )
}

// ============ 服务 ============

function ServiceTab({ sessionId, online }: { sessionId: number; online: boolean }) {
  const canWrite = useCanWrite()
  const q = useEnumList<ServiceItem>(sessionId, 'EnumService', 'services', online)
  const [confirm, setConfirm] = useState<{ name: string } | null>(null)
  const list = q.data ?? []

  const act = async (start: boolean, name: string) => {
    try {
      await sendCommand(sessionId, start ? 'StartService' : 'StopService', { name })
      toast.success(`已${start ? '启动' : '停止'}服务 ${name}`)
      void q.refetch()
    } catch (e) {
      toast.error(`操作失败: ${(e as Error).message}`)
    }
  }

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      <TabToolbar label='系统服务' count={list.length} online={online} isFetching={q.isFetching} onRefresh={() => void q.refetch()} />
      <div className='min-h-0 flex-1 overflow-auto rounded-md border'>
        <Table className='table-fixed'>
          <TableHeader className='sticky top-0 z-10 bg-background'>
            <HeadRow>
              <TableHead className='w-56'>名称</TableHead>
              <TableHead>显示名</TableHead>
              <TableHead className='w-24'>状态</TableHead>
              <TableHead className='w-24'>启动类型</TableHead>
              <TableHead className='w-20 text-right'>PID</TableHead>
              <TableHead className='w-72'>路径</TableHead>
              <TableHead className='w-32 text-end'>操作</TableHead>
            </HeadRow>
          </TableHeader>
          <TableBody>
            {q.isLoading ? (
              <StateRow colSpan={7} text='枚举中…（等待 Agent 往返）' />
            ) : q.isError ? (
              <StateRow colSpan={7} text={`枚举失败：${(q.error as Error).message}`} retry={() => void q.refetch()} />
            ) : list.length === 0 ? (
              <StateRow colSpan={7} text='暂无数据' />
            ) : (
              list.map((s, i) => {
                const running = /run/i.test(String(s.state))
                return (
                  <TableRow key={`${s.name}-${i}`} className='text-xs'>
                    <TableCell className='truncate py-1 font-mono' title={s.name}>
                      {s.name}
                    </TableCell>
                    <TableCell className='truncate py-1' title={s.display_name}>
                      {s.display_name || '—'}
                    </TableCell>
                    <TableCell className='py-1'>{stateBadge(s.state)}</TableCell>
                    <TableCell className='py-1 text-muted-foreground'>{s.start_type || '—'}</TableCell>
                    <TableCell className='py-1 text-right font-mono text-muted-foreground'>{s.process_id || '—'}</TableCell>
                    <TableCell className='truncate py-1 font-mono text-muted-foreground' title={s.binary_path}>
                      {s.binary_path || '—'}
                    </TableCell>
                    <TableCell className='py-1 text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 px-2 text-xs'
                        disabled={!canWrite || !online || running}
                        onClick={() => void act(true, s.name)}
                      >
                        启动
                      </Button>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 px-2 text-xs text-red-500 hover:text-red-400'
                        disabled={!canWrite || !online || !running}
                        onClick={() => setConfirm({ name: s.name })}
                      >
                        停止
                      </Button>
                    </TableCell>
                  </TableRow>
                )
              })
            )}
          </TableBody>
        </Table>
      </div>
      <ConfirmDialog
        open={confirm !== null}
        onOpenChange={(o) => !o && setConfirm(null)}
        title={`停止服务「${confirm?.name}」？`}
        desc='将向目标主机发送 StopService，该服务及其承载功能会立即中止。'
        destructive
        cancelBtnText='取消'
        confirmText='停止服务'
        handleConfirm={() => {
          const name = confirm?.name
          setConfirm(null)
          if (name) void act(false, name)
        }}
      />
    </div>
  )
}

// ============ 网络 ============

function NetworkTab({ sessionId, online }: { sessionId: number; online: boolean }) {
  const q = useEnumList<NetworkItem>(sessionId, 'EnumNetwork', 'connections', online)
  const list = q.data ?? []

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      <TabToolbar label='网络连接' count={list.length} online={online} isFetching={q.isFetching} onRefresh={() => void q.refetch()} />
      <div className='min-h-0 flex-1 overflow-auto rounded-md border'>
        <Table className='table-fixed'>
          <TableHeader className='sticky top-0 z-10 bg-background'>
            <HeadRow>
              <TableHead className='w-52'>本地地址</TableHead>
              <TableHead className='w-52'>远程地址</TableHead>
              <TableHead className='w-32'>状态</TableHead>
              <TableHead className='w-20 text-right'>PID</TableHead>
              <TableHead>进程</TableHead>
            </HeadRow>
          </TableHeader>
          <TableBody>
            {q.isLoading ? (
              <StateRow colSpan={5} text='枚举中…（等待 Agent 往返）' />
            ) : q.isError ? (
              <StateRow colSpan={5} text={`枚举失败：${(q.error as Error).message}`} retry={() => void q.refetch()} />
            ) : list.length === 0 ? (
              <StateRow colSpan={5} text='暂无数据' />
            ) : (
              list.map((c, i) => (
                <TableRow key={`${c.local_addr}:${c.local_port}-${i}`} className='text-xs'>
                  <TableCell className='truncate py-1 font-mono'>
                    {c.local_addr}:{c.local_port}
                  </TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground'>
                    {c.remote_addr ? `${c.remote_addr}:${c.remote_port ?? ''}` : '—'}
                  </TableCell>
                  <TableCell className='py-1'>{stateBadge(c.state)}</TableCell>
                  <TableCell className='py-1 text-right font-mono text-muted-foreground'>{c.pid || '—'}</TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground' title={c.process_name}>
                    {c.process_name || '—'}
                  </TableCell>
                </TableRow>
              ))
            )}
          </TableBody>
        </Table>
      </div>
    </div>
  )
}

// ============ 驱动 ============

function DriverTab({ sessionId, online }: { sessionId: number; online: boolean }) {
  const canWrite = useCanWrite()
  const q = useEnumList<DriverItem>(sessionId, 'EnumDriver', 'drivers', online)
  const [confirm, setConfirm] = useState<{ name: string } | null>(null)
  const list = q.data ?? []

  const act = async (start: boolean, name: string) => {
    try {
      await sendCommand(sessionId, start ? 'StartDriver' : 'StopDriver', { name })
      toast.success(`已${start ? '启动' : '停止'}驱动 ${name}`)
      void q.refetch()
    } catch (e) {
      toast.error(`操作失败: ${(e as Error).message}`)
    }
  }

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      <TabToolbar label='内核/系统驱动' count={list.length} online={online} isFetching={q.isFetching} onRefresh={() => void q.refetch()} />
      <div className='min-h-0 flex-1 overflow-auto rounded-md border'>
        <Table className='table-fixed'>
          <TableHeader className='sticky top-0 z-10 bg-background'>
            <HeadRow>
              <TableHead className='w-56'>名称</TableHead>
              <TableHead>显示名</TableHead>
              <TableHead className='w-24'>状态</TableHead>
              <TableHead className='w-24'>启动类型</TableHead>
              <TableHead className='w-24'>内核驱动</TableHead>
              <TableHead className='w-72'>路径</TableHead>
              <TableHead className='w-32 text-end'>操作</TableHead>
            </HeadRow>
          </TableHeader>
          <TableBody>
            {q.isLoading ? (
              <StateRow colSpan={7} text='枚举中…（等待 Agent 往返）' />
            ) : q.isError ? (
              <StateRow colSpan={7} text={`枚举失败：${(q.error as Error).message}`} retry={() => void q.refetch()} />
            ) : list.length === 0 ? (
              <StateRow colSpan={7} text='暂无数据' />
            ) : (
              list.map((d, i) => {
                const running = /run/i.test(String(d.state))
                return (
                  <TableRow key={`${d.name}-${i}`} className='text-xs'>
                    <TableCell className='truncate py-1 font-mono' title={d.name}>
                      {d.name}
                    </TableCell>
                    <TableCell className='truncate py-1' title={d.display_name}>
                      {d.display_name || '—'}
                    </TableCell>
                    <TableCell className='py-1'>{stateBadge(d.state)}</TableCell>
                    <TableCell className='py-1 text-muted-foreground'>{d.start_type || '—'}</TableCell>
                    <TableCell className='py-1'>{boolBadge(!!d.is_kernel_driver)}</TableCell>
                    <TableCell className='truncate py-1 font-mono text-muted-foreground' title={d.binary_path}>
                      {d.binary_path || '—'}
                    </TableCell>
                    <TableCell className='py-1 text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 px-2 text-xs'
                        disabled={!canWrite || !online || running}
                        onClick={() => void act(true, d.name)}
                      >
                        启动
                      </Button>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 px-2 text-xs text-red-500 hover:text-red-400'
                        disabled={!canWrite || !online || !running}
                        onClick={() => setConfirm({ name: d.name })}
                      >
                        停止
                      </Button>
                    </TableCell>
                  </TableRow>
                )
              })
            )}
          </TableBody>
        </Table>
      </div>
      <ConfirmDialog
        open={confirm !== null}
        onOpenChange={(o) => !o && setConfirm(null)}
        title={`停止驱动「${confirm?.name}」？`}
        desc='将向目标主机发送 StopDriver。内核驱动停止可能导致系统不稳定，请谨慎操作。'
        destructive
        cancelBtnText='取消'
        confirmText='停止驱动'
        handleConfirm={() => {
          const name = confirm?.name
          setConfirm(null)
          if (name) void act(false, name)
        }}
      />
    </div>
  )
}

// ============ 软件 ============

function SoftwareTab({ sessionId, online }: { sessionId: number; online: boolean }) {
  const q = useEnumList<SoftwareItem>(sessionId, 'EnumSoftware', 'software', online)
  const list = q.data ?? []

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      <TabToolbar label='已安装软件' count={list.length} online={online} isFetching={q.isFetching} onRefresh={() => void q.refetch()} />
      <div className='min-h-0 flex-1 overflow-auto rounded-md border'>
        <Table className='table-fixed'>
          <TableHeader className='sticky top-0 z-10 bg-background'>
            <HeadRow>
              <TableHead>软件名</TableHead>
              <TableHead className='w-56'>厂商</TableHead>
              <TableHead className='w-28'>版本</TableHead>
              <TableHead className='w-72'>安装路径</TableHead>
              <TableHead className='w-28'>安装日期</TableHead>
            </HeadRow>
          </TableHeader>
          <TableBody>
            {q.isLoading ? (
              <StateRow colSpan={5} text='枚举中…（等待 Agent 往返）' />
            ) : q.isError ? (
              <StateRow colSpan={5} text={`枚举失败：${(q.error as Error).message}`} retry={() => void q.refetch()} />
            ) : list.length === 0 ? (
              <StateRow colSpan={5} text='暂无数据' />
            ) : (
              list.map((s, i) => (
                <TableRow key={`${s.display_name}-${i}`} className='text-xs'>
                  <TableCell className='truncate py-1' title={s.display_name}>
                    {s.display_name || '—'}
                  </TableCell>
                  <TableCell className='truncate py-1 text-muted-foreground' title={s.publisher}>
                    {s.publisher || '—'}
                  </TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground'>{s.version || '—'}</TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground' title={s.install_location}>
                    {s.install_location || '—'}
                  </TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground'>{s.install_date || '—'}</TableCell>
                </TableRow>
              ))
            )}
          </TableBody>
        </Table>
      </div>
    </div>
  )
}

// ============ 窗口 ============

function WindowTab({ sessionId, online }: { sessionId: number; online: boolean }) {
  const canWrite = useCanWrite()
  const q = useEnumList<WindowItem>(sessionId, 'EnumWindow', 'windows', online)
  const [confirm, setConfirm] = useState<{ handle: number; title: string } | null>(null)
  const list = q.data ?? []

  const act = async (kind: 'top' | 'untop' | 'close', w: WindowItem) => {
    try {
      if (kind === 'close') await sendCommand(sessionId, 'CloseWindow', { handle: w.handle })
      else await sendCommand(sessionId, kind === 'top' ? 'SetTopMostWindow' : 'RemoveTopMostWindow', { handle: w.handle })
      toast.success(kind === 'close' ? '已发送关闭指令' : kind === 'top' ? '已发送置顶指令' : '已发送取消置顶指令')
      void q.refetch()
    } catch (e) {
      toast.error(`操作失败: ${(e as Error).message}`)
    }
  }

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      <TabToolbar label='桌面窗口' count={list.length} online={online} isFetching={q.isFetching} onRefresh={() => void q.refetch()} />
      <div className='min-h-0 flex-1 overflow-auto rounded-md border'>
        <Table className='table-fixed'>
          <TableHeader className='sticky top-0 z-10 bg-background'>
            <HeadRow>
              <TableHead>标题</TableHead>
              <TableHead className='w-48'>类名</TableHead>
              <TableHead className='w-16 text-center'>可见</TableHead>
              <TableHead className='w-16 text-center'>置顶</TableHead>
              <TableHead className='w-16 text-right'>PID</TableHead>
              <TableHead className='w-40'>进程</TableHead>
              <TableHead className='w-40 text-end'>操作</TableHead>
            </HeadRow>
          </TableHeader>
          <TableBody>
            {q.isLoading ? (
              <StateRow colSpan={7} text='枚举中…（等待 Agent 往返）' />
            ) : q.isError ? (
              <StateRow colSpan={7} text={`枚举失败：${(q.error as Error).message}`} retry={() => void q.refetch()} />
            ) : list.length === 0 ? (
              <StateRow colSpan={7} text='暂无数据' />
            ) : (
              list.map((w, i) => (
                <TableRow key={`${w.handle}-${i}`} className='text-xs'>
                  <TableCell className='truncate py-1' title={w.title}>
                    {w.title || '（无标题）'}
                  </TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground' title={w.class_name}>
                    {w.class_name || '—'}
                  </TableCell>
                  <TableCell className='py-1 text-center'>{boolBadge(!!w.is_visible)}</TableCell>
                  <TableCell className='py-1 text-center'>{boolBadge(!!w.is_topmost)}</TableCell>
                  <TableCell className='py-1 text-right font-mono text-muted-foreground'>{w.process_id || '—'}</TableCell>
                  <TableCell className='truncate py-1 font-mono text-muted-foreground' title={w.process_name}>
                    {w.process_name || '—'}
                  </TableCell>
                  <TableCell className='py-1 text-end'>
                    <Button
                      size='sm'
                      variant='ghost'
                      className='h-6 px-2 text-xs'
                      disabled={!canWrite || !online}
                      onClick={() => void act(w.is_topmost ? 'untop' : 'top', w)}
                    >
                      {w.is_topmost ? '取消置顶' : '置顶'}
                    </Button>
                    <Button
                      size='sm'
                      variant='ghost'
                      className='h-6 px-2 text-xs text-red-500 hover:text-red-400'
                      disabled={!canWrite || !online}
                      onClick={() => setConfirm({ handle: w.handle, title: w.title })}
                    >
                      关闭
                    </Button>
                  </TableCell>
                </TableRow>
              ))
            )}
          </TableBody>
        </Table>
      </div>
      <ConfirmDialog
        open={confirm !== null}
        onOpenChange={(o) => !o && setConfirm(null)}
        title={`关闭窗口「${confirm?.title || confirm?.handle}」？`}
        desc='将向目标主机发送 CloseWindow（强制关闭该窗口，窗口内未保存的数据将丢失）。'
        destructive
        cancelBtnText='取消'
        confirmText='关闭窗口'
        handleConfirm={() => {
          const w = list.find((x) => x.handle === confirm?.handle)
          setConfirm(null)
          if (w) void act('close', w)
        }}
      />
    </div>
  )
}
