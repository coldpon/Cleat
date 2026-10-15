// 操作员管理(真实对接 ): REST /api/operators(列表/新增/启停/删除/改密)。
// 新增需提供该操作员的认证 token(与 TeamServer operators 配置对齐)。
import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { KeyRound, Plus, RefreshCw, Trash2, Users } from 'lucide-react'
import { toast } from 'sonner'
import { cn } from '@/lib/utils'
import { addOperator, deleteOperator, listOperators, toggleOperator, type OperatorRow } from '@/lib/cleat-rest'
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

const ROLE_LABEL: Record<OperatorRow['role'], string> = {
  admin: '管理员',
  operator: '操作员',
  viewer: '观察者',
}

const ROLE_COLORS: Record<OperatorRow['role'], string> = {
  admin: 'border-red-500/40 bg-red-500/10 text-red-400',
  operator: 'border-sky-500/40 bg-sky-500/10 text-sky-400',
  viewer: 'border-zinc-500/40 bg-zinc-500/10 text-zinc-400',
}

export function Operators() {
  const qc = useQueryClient()
  const [addOpen, setAddOpen] = useState(false)
  const [form, setForm] = useState({ username: '', role: 'operator' as OperatorRow['role'], token: '' })

  const q = useQuery({ queryKey: ['operators'], queryFn: listOperators, refetchInterval: 15000 })
  const list = q.data?.operators ?? []

  const invalidate = () => void qc.invalidateQueries({ queryKey: ['operators'] })

  const add = useMutation({
    mutationFn: () => addOperator({ username: form.username.trim(), role: form.role, token: form.token.trim() }),
    onSuccess: () => {
      toast.success(`操作员 ${form.username.trim()} 已创建`)
      setAddOpen(false)
      setForm({ username: '', role: 'operator', token: '' })
      invalidate()
    },
    onError: (e) => toast.error(`创建失败: ${(e as Error).message}`),
  })

  const toggle = useMutation({
    mutationFn: toggleOperator,
    onSuccess: () => {
      toast.success('状态已切换')
      invalidate()
    },
    onError: (e) => toast.error(`切换失败: ${(e as Error).message}`),
  })

  const del = useMutation({
    mutationFn: deleteOperator,
    onSuccess: () => {
      toast.success('操作员已删除')
      invalidate()
    },
    onError: (e) => toast.error(`删除失败: ${(e as Error).message}`),
  })

  const activeCount = list.filter((o) => o.active).length

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
              <Users className='size-5' />
              操作员
            </h2>
            <p className='text-muted-foreground'>
              共 {list.length} 人 · 启用 <span className='font-medium text-emerald-500'>{activeCount}</span> 人
            </p>
          </div>
          <div className='flex gap-2'>
            <Button size='sm' variant='outline' onClick={() => void q.refetch()}>
              <RefreshCw className={cn('size-4', q.isFetching && 'animate-spin')} />
              刷新
            </Button>
            <Button size='sm' onClick={() => setAddOpen(true)}>
              <Plus className='size-4' />
              新增操作员
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>账户</TableHead>
                <TableHead className='w-24'>角色</TableHead>
                <TableHead className='w-24'>状态</TableHead>
                <TableHead className='w-40 text-end'>操作</TableHead>
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
                    暂无操作员
                  </TableCell>
                </TableRow>
              ) : (
                list.map((o) => (
                  <TableRow key={o.username} className='h-9'>
                    <TableCell>
                      <span className='flex items-center gap-2'>
                        <KeyRound className='size-3.5 text-muted-foreground' />
                        <span className='font-mono text-sm'>{o.username}</span>
                      </span>
                    </TableCell>
                    <TableCell>
                      <Badge variant='outline' className={cn('rounded-sm font-mono text-[10px]', ROLE_COLORS[o.role])}>
                        {ROLE_LABEL[o.role]}
                      </Badge>
                    </TableCell>
                    <TableCell>
                      <span className='flex items-center gap-1.5 text-xs'>
                        <span
                          className={cn(
                            'inline-block size-2 rounded-full',
                            o.active ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]' : 'bg-muted-foreground/40'
                          )}
                        />
                        {o.active ? '启用' : '禁用'}
                      </span>
                    </TableCell>
                    <TableCell className='text-end'>
                      <div className='flex justify-end gap-1'>
                        <Button size='sm' variant='ghost' className='h-7 text-xs' onClick={() => toggle.mutate(o.username)}>
                          {o.active ? '禁用' : '启用'}
                        </Button>
                        <Button
                          size='sm'
                          variant='ghost'
                          className='h-7 gap-1 text-xs text-red-400 hover:text-red-400'
                          onClick={() => {
                            if (window.confirm(`删除操作员 ${o.username}？`)) del.mutate(o.username)
                          }}
                        >
                          <Trash2 className='size-3.5' />
                          删除
                        </Button>
                      </div>
                    </TableCell>
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </div>
      </Main>

      {/* 新增操作员 */}
      <Dialog open={addOpen} onOpenChange={setAddOpen}>
        <DialogContent className='sm:max-w-sm'>
          <DialogHeader>
            <DialogTitle>新增操作员</DialogTitle>
            <DialogDescription>
              需提供认证 token(与 TeamServer 侧 operators 配置一致); 仅 admin 可新增
            </DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <Input placeholder='账户名 *' value={form.username} onChange={(e) => setForm({ ...form, username: e.target.value })} />
            <Select value={form.role} onValueChange={(v) => setForm({ ...form, role: v as OperatorRow['role'] })}>
              <SelectTrigger>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {(Object.keys(ROLE_LABEL) as OperatorRow['role'][]).map((r) => (
                  <SelectItem key={r} value={r}>
                    {ROLE_LABEL[r]}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
            <Input placeholder='认证 Token *' value={form.token} onChange={(e) => setForm({ ...form, token: e.target.value })} />
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setAddOpen(false)}>
              取消
            </Button>
            <Button disabled={!form.username.trim() || !form.token.trim() || add.isPending} onClick={() => add.mutate()}>
              创建
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}
