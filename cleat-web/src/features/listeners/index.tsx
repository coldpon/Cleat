// 监听器管理(真实对接 ): REST /api/listeners(列表/创建/删除)。
// 创建参数: protocol(tcp/https/dns/wss/quic) / address / port / dns 必填 domain / wss 可选 ws_path。
// 无启停接口(后端未提供 toggle) —— 删除即停止。
import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Plus, Radio, RefreshCw, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { cn } from '@/lib/utils'
import { createListener, deleteListener, listListeners, type ListenerRow } from '@/lib/cleat-rest'
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

export function Listeners() {
  const qc = useQueryClient()
  const [createOpen, setCreateOpen] = useState(false)
  const [form, setForm] = useState({ protocol: 'tcp', address: '0.0.0.0', port: '8443', domain: '', ws_path: '' })

  const q = useQuery({
    queryKey: ['listeners'],
    queryFn: listListeners,
    refetchInterval: 10000,
  })
  const list = q.data?.listeners ?? []
  const running = list.filter((l) => l.status === 'running').length

  const create = useMutation({
    mutationFn: () =>
      createListener({
        protocol: form.protocol,
        address: form.address.trim() || '0.0.0.0',
        port: Number(form.port),
        ...(form.protocol === 'dns' ? { domain: form.domain.trim() } : {}),
        ...(form.protocol === 'wss' && form.ws_path.trim() ? { ws_path: form.ws_path.trim() } : {}),
      }),
    onSuccess: () => {
      toast.success('监听器已创建')
      setCreateOpen(false)
      void qc.invalidateQueries({ queryKey: ['listeners'] })
    },
    onError: (e) => toast.error(`创建失败: ${(e as Error).message}`),
  })

  const del = useMutation({
    mutationFn: deleteListener,
    onSuccess: () => {
      toast.success('监听器已删除(停止)')
      void qc.invalidateQueries({ queryKey: ['listeners'] })
    },
    onError: (e) => toast.error(`删除失败: ${(e as Error).message}`),
  })

  const portValid = Number(form.port) > 0 && Number(form.port) <= 65535
  const dnsOk = form.protocol !== 'dns' || !!form.domain.trim()

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
              <Radio className='size-5' />
              监听器
            </h2>
            <p className='text-muted-foreground'>
              运行中 <span className='font-medium text-emerald-500'>{running}</span> / {list.length} 个监听器
            </p>
          </div>
          <div className='flex gap-2'>
            <Button size='sm' variant='outline' onClick={() => void q.refetch()}>
              <RefreshCw className={cn('size-4', q.isFetching && 'animate-spin')} />
              刷新
            </Button>
            <Button size='sm' onClick={() => setCreateOpen(true)}>
              <Plus className='size-4' />
              新建监听器
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead className='w-24'>协议</TableHead>
                <TableHead>监听地址</TableHead>
                <TableHead className='w-28'>状态</TableHead>
                <TableHead className='w-24 text-end'>操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {q.isLoading ? (
                <TableRow>
                  <TableCell colSpan={4} className='h-24 text-center text-sm text-muted-foreground'>
                    加载中…
                  </TableCell>
                </TableRow>
              ) : q.isError ? (
                <TableRow>
                  <TableCell colSpan={4} className='h-24 text-center text-sm text-muted-foreground'>
                    加载失败：{(q.error as Error).message}
                    <Button size='sm' variant='outline' className='ms-3 h-7 text-xs' onClick={() => void q.refetch()}>
                      重试
                    </Button>
                  </TableCell>
                </TableRow>
              ) : list.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={4} className='h-24 text-center text-sm text-muted-foreground'>
                    暂无监听器 · 点击"新建监听器"开启回调端口
                  </TableCell>
                </TableRow>
              ) : (
                list.map((l: ListenerRow) => (
                  <TableRow key={l.address} className='h-9'>
                    <TableCell>
                      <Badge variant='secondary' className='rounded-sm font-mono text-[10px] uppercase'>
                        {l.protocol}
                      </Badge>
                    </TableCell>
                    <TableCell className='font-mono text-xs text-muted-foreground'>
                      {l.address}
                      {l.port ? `:${l.port}` : ''}
                    </TableCell>
                    <TableCell>
                      <span className='flex items-center gap-1.5 text-xs'>
                        <span
                          className={cn(
                            'inline-block size-2 rounded-full',
                            l.status === 'running'
                              ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]'
                              : 'bg-muted-foreground/40'
                          )}
                        />
                        {l.status === 'running' ? '运行中' : '已停止'}
                      </span>
                    </TableCell>
                    <TableCell className='text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-7 gap-1 text-xs text-red-400 hover:text-red-400'
                        onClick={() => {
                          if (window.confirm(`删除监听器 ${l.address}？(即停止监听)`)) del.mutate(l.address)
                        }}
                      >
                        <Trash2 className='size-3.5' />
                        删除
                      </Button>
                    </TableCell>
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </div>
      </Main>

      {/* 新建监听器 */}
      <Dialog open={createOpen} onOpenChange={setCreateOpen}>
        <DialogContent className='sm:max-w-md'>
          <DialogHeader>
            <DialogTitle>新建监听器</DialogTitle>
            <DialogDescription>
              {form.protocol === 'dns'
                ? 'DNS 隧道需填写查询域名(与 Agent dns_domain 一致)'
                : form.protocol === 'wss'
                  ? 'WSS 可自定义升级路径(默认 /ws, 需与 Agent wss_url 一致)'
                  : 'https/dns/quic 由 beacon 请求驱动监听'}
            </DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <div className='grid grid-cols-2 gap-2'>
              <Select value={form.protocol} onValueChange={(v) => setForm({ ...form, protocol: v })}>
                <SelectTrigger>
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {['tcp', 'https', 'dns', 'wss', 'quic'].map((p) => (
                    <SelectItem key={p} value={p}>
                      {p}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <Input
                placeholder='端口 *'
                inputMode='numeric'
                value={form.port}
                onChange={(e) => setForm({ ...form, port: e.target.value.replace(/\D/g, '') })}
              />
            </div>
            <Input placeholder='监听地址(默认 0.0.0.0)' value={form.address} onChange={(e) => setForm({ ...form, address: e.target.value })} />
            {form.protocol === 'dns' && (
              <Input placeholder='查询域名 * (如 c2.corp.local)' value={form.domain} onChange={(e) => setForm({ ...form, domain: e.target.value })} />
            )}
            {form.protocol === 'wss' && (
              <Input placeholder='WS 路径(默认 /ws)' value={form.ws_path} onChange={(e) => setForm({ ...form, ws_path: e.target.value })} />
            )}
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setCreateOpen(false)}>
              取消
            </Button>
            <Button disabled={!portValid || !dnsOk || create.isPending} onClick={() => create.mutate()}>
              创建
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}
