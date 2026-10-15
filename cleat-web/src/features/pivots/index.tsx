// 隧道管理(真实对接 ):
//   SOCKS 出口 —— REST /api/pivots(列表/创建/删除);
//   反向端口转发 —— REST /api/rportfwds(Agent 在受害主机监听, 入站经 C2 通道转发到目标)。
// 后端目前提供这两类隧道; 演示页里的"会话借道/正向转发"为未来能力占位, 已移除。
import { useMemo, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Network, Plus, RefreshCw, Trash2, Waypoints } from 'lucide-react'
import { toast } from 'sonner'
import { listSessions } from '@/lib/cleat-api'
import { cn } from '@/lib/utils'
import {
  createPivot,
  createRPortFwd,
  deletePivot,
  deleteRPortFwd,
  listPivots,
  listRPortFwds,
  type PivotRow,
  type RPortFwdRow,
} from '@/lib/cleat-rest'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

interface TunnelRow {
  key: string
  type: 'SOCKS5' | '反向转发'
  local: string
  target: string
  sessionId: number
  running: boolean
  remove: () => Promise<void>
}

export function Pivots() {
  const qc = useQueryClient()
  const [socksOpen, setSocksOpen] = useState(false)
  const [fwdOpen, setFwdOpen] = useState(false)
  const [socksForm, setSocksForm] = useState({ sessionId: '', port: '1080' })
  const [fwdForm, setFwdForm] = useState({ sessionId: '', listenPort: '', targetHost: '', targetPort: '' })

  // 在线会话(SOCKS/转发的出口选择)
  const { data: sessData } = useQuery({ queryKey: ['sessions'], queryFn: listSessions, refetchInterval: 5000 })
  const onlineSessions = (sessData?.sessions ?? []).filter((s) => s.online)

  const pivotsQ = useQuery({ queryKey: ['pivots'], queryFn: listPivots, refetchInterval: 10000 })
  const fwdQ = useQuery({ queryKey: ['rportfwds'], queryFn: listRPortFwds, refetchInterval: 10000 })

  const rows = useMemo<TunnelRow[]>(() => {
    const socks: TunnelRow[] = (pivotsQ.data?.pivots ?? []).map((p: PivotRow) => ({
      key: `socks-${p.local_addr}`,
      type: 'SOCKS5',
      local: p.local_addr,
      target: `经 会话 #${p.session_id}`,
      sessionId: p.session_id,
      running: p.status === 'running',
      remove: async () => {
        await deletePivot(p.local_addr)
      },
    }))
    const fwds: TunnelRow[] = (fwdQ.data?.rportfwds ?? []).map((r: RPortFwdRow) => ({
      key: `fwd-${r.session_id}-${r.listen_port}`,
      type: '反向转发',
      local: `0.0.0.0:${r.listen_port}`,
      target: `${r.target_host}:${r.target_port}`,
      sessionId: r.session_id,
      running: true, // 登记即运行(后端 RportfwdInfo 语义)
      remove: async () => {
        await deleteRPortFwd(r.session_id, r.listen_port)
      },
    }))
    return [...socks, ...fwds]
  }, [pivotsQ.data, fwdQ.data])

  const invalidate = () => {
    void qc.invalidateQueries({ queryKey: ['pivots'] })
    void qc.invalidateQueries({ queryKey: ['rportfwds'] })
  }

  const createSocks = useMutation({
    mutationFn: () =>
      createPivot({ session_id: Number(socksForm.sessionId), port: Number(socksForm.port) }),
    onSuccess: (p) => {
      toast.success(`SOCKS5 出口已建立: ${p.local_addr || `:${socksForm.port}`}`)
      setSocksOpen(false)
      invalidate()
    },
    onError: (e) => toast.error(`创建失败: ${(e as Error).message}`),
  })

  const createFwd = useMutation({
    mutationFn: () =>
      createRPortFwd({
        session_id: Number(fwdForm.sessionId),
        listen_port: Number(fwdForm.listenPort),
        target_host: fwdForm.targetHost.trim(),
        target_port: Number(fwdForm.targetPort),
      }),
    onSuccess: () => {
      toast.success('反向端口转发已建立')
      setFwdOpen(false)
      invalidate()
    },
    onError: (e) => toast.error(`创建失败: ${(e as Error).message}`),
  })

  const remove = useMutation({
    mutationFn: async (t: TunnelRow) => t.remove(),
    onSuccess: () => {
      toast.success('隧道已停止')
      invalidate()
    },
    onError: (e) => toast.error(`停止失败: ${(e as Error).message}`),
  })

  const runningCount = rows.filter((t) => t.running).length
  const socksValid = !!socksForm.sessionId && Number(socksForm.port) > 0
  const fwdValid = !!fwdForm.sessionId && Number(fwdForm.listenPort) > 0 && !!fwdForm.targetHost.trim() && Number(fwdForm.targetPort) > 0

  return (
    <>
      <Header fixed>
        <Search className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main>
        <div className='mb-4 flex flex-wrap items-end justify-between gap-2'>
          <div>
            <h2 className='flex items-center gap-2 text-2xl font-bold tracking-tight'>
              <Waypoints className='size-5' />
              隧道
            </h2>
            <p className='text-muted-foreground'>
              SOCKS 出口 / 反向端口转发 共 {rows.length} 条 · 活跃{' '}
              <span className='font-medium text-emerald-500'>{runningCount}</span> 条
            </p>
          </div>
          <div className='flex gap-2'>
            <Button size='sm' variant='outline' onClick={() => { void pivotsQ.refetch(); void fwdQ.refetch() }}>
              <RefreshCw className={cn('size-4', (pivotsQ.isFetching || fwdQ.isFetching) && 'animate-spin')} />
              刷新
            </Button>
            <Button size='sm' variant='outline' onClick={() => setFwdOpen(true)} disabled={onlineSessions.length === 0}>
              <Network className='size-4' />
              反向转发
            </Button>
            <Button size='sm' onClick={() => setSocksOpen(true)} disabled={onlineSessions.length === 0}>
              <Plus className='size-4' />
              SOCKS 出口
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead className='w-24'>类型</TableHead>
                <TableHead>本地监听</TableHead>
                <TableHead>目标 / 出口</TableHead>
                <TableHead className='w-20'>会话</TableHead>
                <TableHead className='w-24'>状态</TableHead>
                <TableHead className='w-20 text-end'>操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {pivotsQ.isLoading || fwdQ.isLoading ? (
                <TableRow>
                  <TableCell colSpan={6} className='h-24 text-center text-sm text-muted-foreground'>
                    加载中…
                  </TableCell>
                </TableRow>
              ) : rows.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={6} className='h-24 text-center text-sm text-muted-foreground'>
                    暂无隧道{onlineSessions.length === 0 ? '（需要在线会话作为出口）' : ''}
                  </TableCell>
                </TableRow>
              ) : (
                rows.map((t) => (
                  <TableRow key={t.key} className='h-9'>
                    <TableCell>
                      <Badge variant='secondary' className='rounded-sm font-mono text-[10px]'>
                        {t.type}
                      </Badge>
                    </TableCell>
                    <TableCell className='font-mono text-sm'>{t.local}</TableCell>
                    <TableCell className='text-sm text-muted-foreground'>{t.target}</TableCell>
                    <TableCell className='font-mono text-xs text-muted-foreground'>#{t.sessionId}</TableCell>
                    <TableCell>
                      <span className='flex items-center gap-1.5 text-xs'>
                        <span
                          className={cn(
                            'inline-block size-2 rounded-full',
                            t.running ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]' : 'bg-muted-foreground/40'
                          )}
                        />
                        {t.running ? '活跃' : '—'}
                      </span>
                    </TableCell>
                    <TableCell className='text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 w-6 p-0 text-muted-foreground hover:text-red-400'
                        title='停止并删除'
                        onClick={() => {
                          if (window.confirm(`停止隧道 ${t.local}？`)) remove.mutate(t)
                        }}
                      >
                        <Trash2 className='size-3.5' />
                      </Button>
                    </TableCell>
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </div>
      </Main>

      {/* 新建 SOCKS 出口 */}
      <Dialog open={socksOpen} onOpenChange={setSocksOpen}>
        <DialogContent className='sm:max-w-sm'>
          <DialogHeader>
            <DialogTitle>新建 SOCKS 出口</DialogTitle>
            <DialogDescription>经指定在线会话建立 SOCKS5 代理(本地回环监听)</DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <Select value={socksForm.sessionId} onValueChange={(v) => setSocksForm({ ...socksForm, sessionId: v })}>
              <SelectTrigger>
                <SelectValue placeholder='选择出口会话 *' />
              </SelectTrigger>
              <SelectContent>
                {onlineSessions.map((s) => (
                  <SelectItem key={s.id} value={String(s.id)}>
                    #{s.id} {s.hostname}（{s.local_ip}）
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Input
              placeholder='本地端口 * (如 1080)'
              inputMode='numeric'
              value={socksForm.port}
              onChange={(e) => setSocksForm({ ...socksForm, port: e.target.value.replace(/\D/g, '') })}
            />
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setSocksOpen(false)}>
              取消
            </Button>
            <Button disabled={!socksValid || createSocks.isPending} onClick={() => createSocks.mutate()}>
              建立
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      {/* 新建反向端口转发 */}
      <Dialog open={fwdOpen} onOpenChange={setFwdOpen}>
        <DialogContent className='sm:max-w-sm'>
          <DialogHeader>
            <DialogTitle>新建反向端口转发</DialogTitle>
            <DialogDescription>Agent 在受害主机监听 listen_port, 入站流量经 C2 通道转发到目标</DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <Select value={fwdForm.sessionId} onValueChange={(v) => setFwdForm({ ...fwdForm, sessionId: v })}>
              <SelectTrigger>
                <SelectValue placeholder='选择通道会话 *' />
              </SelectTrigger>
              <SelectContent>
                {onlineSessions.map((s) => (
                  <SelectItem key={s.id} value={String(s.id)}>
                    #{s.id} {s.hostname}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Input
              placeholder='受害主机监听端口 * (如 8081)'
              inputMode='numeric'
              value={fwdForm.listenPort}
              onChange={(e) => setFwdForm({ ...fwdForm, listenPort: e.target.value.replace(/\D/g, '') })}
            />
            <div className='grid grid-cols-2 gap-2'>
              <Input placeholder='目标主机 * (如 10.0.15.11)' value={fwdForm.targetHost} onChange={(e) => setFwdForm({ ...fwdForm, targetHost: e.target.value })} />
              <Input
                placeholder='目标端口 *'
                inputMode='numeric'
                value={fwdForm.targetPort}
                onChange={(e) => setFwdForm({ ...fwdForm, targetPort: e.target.value.replace(/\D/g, '') })}
              />
            </div>
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setFwdOpen(false)}>
              取消
            </Button>
            <Button disabled={!fwdValid || createFwd.isPending} onClick={() => createFwd.mutate()}>
              建立
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}
