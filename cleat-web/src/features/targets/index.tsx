// 目标管理(真实对接 ): REST /api/targets(列表/新增/状态更新/删除)。
// TargetRow: ip/hostname/os/ports/protocol/status(new|scanned|compromised|closed)/tags/notes。
import { useMemo, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Plus, Search, Target, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import {
  addTarget,
  deleteTarget,
  listTargets,
  updateTarget,
  type TargetStatus,
} from '@/lib/cleat-rest'
import { cn } from '@/lib/utils'
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
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import { Input } from '@/components/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search as GlobalSearch } from '@/components/search'

const STATUS_LABEL: Record<TargetStatus, string> = {
  new: '新发现',
  scanned: '已侦察',
  compromised: '已拿下',
  closed: '已关闭',
}

const STATUS_COLORS: Record<TargetStatus, string> = {
  new: 'border-sky-500/40 bg-sky-500/10 text-sky-400',
  scanned: 'border-amber-500/40 bg-amber-500/10 text-amber-400',
  compromised: 'border-emerald-500/40 bg-emerald-500/10 text-emerald-400',
  closed: 'border-zinc-500/40 bg-zinc-500/10 text-zinc-400',
}

export function Targets() {
  const qc = useQueryClient()
  const [kw, setKw] = useState('')
  const [addOpen, setAddOpen] = useState(false)
  const [form, setForm] = useState({ ip: '', hostname: '', os: '', ports: '', protocol: 'tcp', notes: '' })

  const q = useQuery({
    queryKey: ['targets'],
    queryFn: listTargets,
    refetchInterval: 15000,
  })
  const list = q.data?.targets ?? []

  const invalidate = () => void qc.invalidateQueries({ queryKey: ['targets'] })

  const add = useMutation({
    mutationFn: () =>
      addTarget({
        ip: form.ip.trim(),
        hostname: form.hostname.trim(),
        os: form.os.trim(),
        ports: form.ports.trim(),
        protocol: form.protocol.trim() || 'tcp',
        notes: form.notes.trim(),
      }),
    onSuccess: () => {
      toast.success('目标已添加')
      setAddOpen(false)
      setForm({ ip: '', hostname: '', os: '', ports: '', protocol: 'tcp', notes: '' })
      invalidate()
    },
    onError: (e) => toast.error(`添加失败: ${(e as Error).message}`),
  })

  const setStatus = useMutation({
    mutationFn: ({ id, status }: { id: number; status: TargetStatus }) => updateTarget(id, { status }),
    onSuccess: () => {
      toast.success('状态已更新')
      invalidate()
    },
    onError: (e) => toast.error(`更新失败: ${(e as Error).message}`),
  })

  const del = useMutation({
    mutationFn: deleteTarget,
    onSuccess: () => {
      toast.success('目标已删除')
      invalidate()
    },
    onError: (e) => toast.error(`删除失败: ${(e as Error).message}`),
  })

  const rows = useMemo(() => {
    const k = kw.trim().toLowerCase()
    if (!k) return list
    return list.filter(
      (t) =>
        t.ip.includes(k) ||
        t.hostname.toLowerCase().includes(k) ||
        t.os.toLowerCase().includes(k) ||
        (t.tags || '').toLowerCase().includes(k)
    )
  }, [list, kw])

  const alive = list.filter((t) => t.status !== 'closed').length

  return (
    <>
      <Header fixed>
        <GlobalSearch className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main>
        <div className='mb-4 flex flex-wrap items-end justify-between gap-2'>
          <div>
            <h2 className='flex items-center gap-2 text-2xl font-bold tracking-tight'>
              <Target className='size-5' />
              目标
            </h2>
            <p className='text-muted-foreground'>
              共 {list.length} 个目标 · 未关闭 <span className='font-medium text-emerald-500'>{alive}</span> 个
            </p>
          </div>
          <div className='flex gap-2'>
            <div className='relative'>
              <Search className='absolute start-2.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground' />
              <Input placeholder='过滤 IP / 主机名 / 标签…' value={kw} onChange={(e) => setKw(e.target.value)} className='h-8 w-64 ps-8' />
            </div>
            <Button size='sm' className='h-8 gap-1 text-xs' onClick={() => setAddOpen(true)}>
              <Plus className='size-3.5' />
              添加目标
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table className='table-fixed'>
            <TableHeader>
              <TableRow>
                <TableHead className='w-36'>IP</TableHead>
                <TableHead className='w-40'>主机名</TableHead>
                <TableHead className='w-40'>操作系统</TableHead>
                <TableHead className='w-28'>端口</TableHead>
                <TableHead className='w-24'>状态</TableHead>
                <TableHead>备注 / 标签</TableHead>
                <TableHead className='w-16 text-end'>操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {q.isLoading ? (
                <TableRow>
                  <TableCell colSpan={7} className='h-24 text-center text-sm text-muted-foreground'>
                    加载中…
                  </TableCell>
                </TableRow>
              ) : q.isError ? (
                <TableRow>
                  <TableCell colSpan={7} className='h-24 text-center text-sm text-muted-foreground'>
                    加载失败：{(q.error as Error).message}
                    <Button size='sm' variant='outline' className='ms-3 h-7 text-xs' onClick={() => void q.refetch()}>
                      重试
                    </Button>
                  </TableCell>
                </TableRow>
              ) : rows.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={7} className='h-24 text-center text-sm text-muted-foreground'>
                    暂无目标 · 手工添加或由侦察流程登记
                  </TableCell>
                </TableRow>
              ) : (
                rows.map((t) => (
                  <TableRow key={t.id} className='h-9'>
                    <TableCell className='truncate font-mono text-xs' title={t.ip}>
                      {t.ip}
                    </TableCell>
                    <TableCell className='truncate text-sm' title={t.hostname}>
                      {t.hostname || '—'}
                    </TableCell>
                    <TableCell className='truncate text-xs text-muted-foreground' title={t.os}>
                      {t.os || '—'}
                    </TableCell>
                    <TableCell className='truncate font-mono text-xs text-muted-foreground' title={t.ports}>
                      {t.ports || '—'}
                    </TableCell>
                    <TableCell>
                      <DropdownMenu>
                        <DropdownMenuTrigger asChild>
                          <Badge
                            variant='outline'
                            className={cn('cursor-pointer rounded-sm font-mono text-[10px]', STATUS_COLORS[t.status])}
                          >
                            {STATUS_LABEL[t.status]}
                          </Badge>
                        </DropdownMenuTrigger>
                        <DropdownMenuContent align='start'>
                          <DropdownMenuLabel className='text-xs text-muted-foreground'>更新状态</DropdownMenuLabel>
                          {(Object.keys(STATUS_LABEL) as TargetStatus[]).map((st) => (
                            <DropdownMenuItem key={st} onClick={() => setStatus.mutate({ id: t.id, status: st })}>
                              {STATUS_LABEL[st]}
                            </DropdownMenuItem>
                          ))}
                        </DropdownMenuContent>
                      </DropdownMenu>
                    </TableCell>
                    <TableCell className='truncate text-xs text-muted-foreground' title={t.notes || t.tags}>
                      {t.notes || t.tags || '—'}
                    </TableCell>
                    <TableCell className='text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 w-6 p-0 text-muted-foreground hover:text-red-400'
                        title='删除'
                        onClick={() => {
                          if (window.confirm(`删除目标 ${t.ip}${t.hostname ? ` (${t.hostname})` : ''}？`)) del.mutate(t.id)
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

      {/* 添加目标 */}
      <Dialog open={addOpen} onOpenChange={setAddOpen}>
        <DialogContent className='sm:max-w-md'>
          <DialogHeader>
            <DialogTitle>添加目标</DialogTitle>
            <DialogDescription>登记侦察发现的主机(IP 必填)</DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <Input placeholder='IP *' value={form.ip} onChange={(e) => setForm({ ...form, ip: e.target.value })} />
            <div className='grid grid-cols-2 gap-2'>
              <Input placeholder='主机名' value={form.hostname} onChange={(e) => setForm({ ...form, hostname: e.target.value })} />
              <Input placeholder='操作系统' value={form.os} onChange={(e) => setForm({ ...form, os: e.target.value })} />
            </div>
            <div className='grid grid-cols-2 gap-2'>
              <Input placeholder='端口(如 80,443)' value={form.ports} onChange={(e) => setForm({ ...form, ports: e.target.value })} />
              <Input placeholder='协议(默认 tcp)' value={form.protocol} onChange={(e) => setForm({ ...form, protocol: e.target.value })} />
            </div>
            <Input placeholder='备注' value={form.notes} onChange={(e) => setForm({ ...form, notes: e.target.value })} />
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setAddOpen(false)}>
              取消
            </Button>
            <Button disabled={!form.ip.trim() || add.isPending} onClick={() => add.mutate()}>
              添加
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}
