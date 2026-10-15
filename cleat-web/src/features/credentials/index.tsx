// 凭据库(真实对接 ): REST /api/credentials(列表/新增/删除)。
// 字段对齐后端 Credential: username/realm/cred_type/secret/host/source/agent_id/tag/created_at。
import { useMemo, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { KeyRound, Plus, Search, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { addCredential, deleteCredential, listCredentials, type Credential } from '@/lib/cleat-rest'
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
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '@/components/ui/dropdown-menu'
import { Input } from '@/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search as GlobalSearch } from '@/components/search'

const TYPE_LABEL: Record<Credential['cred_type'], string> = {
  password: '明文',
  hash: '哈希',
  token: '令牌',
  ticket: '票据',
  key: '密钥',
  other: '其他',
}

const TYPE_COLORS: Record<Credential['cred_type'], string> = {
  password: 'border-red-500/40 bg-red-500/10 text-red-400',
  hash: 'border-sky-500/40 bg-sky-500/10 text-sky-400',
  token: 'border-violet-500/40 bg-violet-500/10 text-violet-400',
  ticket: 'border-amber-500/40 bg-amber-500/10 text-amber-400',
  key: 'border-teal-500/40 bg-teal-500/10 text-teal-400',
  other: 'border-zinc-500/40 bg-zinc-500/10 text-zinc-400',
}

function fmtTime(ms: number): string {
  if (!ms) return '—'
  const d = new Date(ms)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`
}

export function Credentials() {
  const qc = useQueryClient()
  const [kw, setKw] = useState('')
  const [addOpen, setAddOpen] = useState(false)
  const [form, setForm] = useState({ username: '', realm: '', cred_type: 'password' as Credential['cred_type'], secret: '', host: '', tag: '' })

  const q = useQuery({
    queryKey: ['credentials'],
    queryFn: listCredentials,
    refetchInterval: 15000,
  })
  const creds = q.data?.credentials ?? []

  const del = useMutation({
    mutationFn: deleteCredential,
    onSuccess: () => {
      toast.success('已删除凭据')
      void qc.invalidateQueries({ queryKey: ['credentials'] })
    },
    onError: (e) => toast.error(`删除失败: ${(e as Error).message}`),
  })

  const add = useMutation({
    mutationFn: () =>
      addCredential({
        username: form.username.trim(),
        realm: form.realm.trim(),
        cred_type: form.cred_type,
        secret: form.secret,
        host: form.host.trim(),
        source: '手工录入',
        tag: form.tag.trim(),
      }),
    onSuccess: () => {
      toast.success('凭据已录入')
      setAddOpen(false)
      setForm({ username: '', realm: '', cred_type: 'password', secret: '', host: '', tag: '' })
      void qc.invalidateQueries({ queryKey: ['credentials'] })
    },
    onError: (e) => toast.error(`录入失败: ${(e as Error).message}`),
  })

  const rows = useMemo(() => {
    const k = kw.trim().toLowerCase()
    if (!k) return creds
    return creds.filter(
      (c) =>
        c.host.toLowerCase().includes(k) ||
        c.username.toLowerCase().includes(k) ||
        c.source.toLowerCase().includes(k) ||
        c.realm.toLowerCase().includes(k)
    )
  }, [creds, kw])

  const hosts = new Set(creds.map((c) => c.host || `#${c.agent_id}`)).size

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
              <KeyRound className='size-5' />
              凭据
            </h2>
            <p className='text-muted-foreground'>
              共 {creds.length} 条凭据 · 来自 {hosts} 台主机
            </p>
          </div>
          <div className='flex gap-2'>
            <div className='relative'>
              <Search className='absolute start-2.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground' />
              <Input placeholder='过滤主机 / 账户 / 来源…' value={kw} onChange={(e) => setKw(e.target.value)} className='h-8 w-64 ps-8' />
            </div>
            <Button size='sm' className='h-8 gap-1 text-xs' onClick={() => setAddOpen(true)}>
              <Plus className='size-3.5' />
              录入
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table className='table-fixed'>
            <TableHeader>
              <TableRow>
                <TableHead className='w-40'>主机</TableHead>
                <TableHead className='w-44'>账户</TableHead>
                <TableHead>口令 / 哈希</TableHead>
                <TableHead className='w-20'>类型</TableHead>
                <TableHead className='w-32'>来源</TableHead>
                <TableHead className='w-32'>时间</TableHead>
                <TableHead className='w-12' />
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
                    暂无凭据（CredentialCollect 采集结果会自动入库）
                  </TableCell>
                </TableRow>
              ) : (
                rows.map((c) => (
                  <TableRow key={c.id} className='h-9'>
                    <TableCell className='truncate font-medium' title={c.host}>
                      {c.host || <span className='text-muted-foreground'>#{c.agent_id || '—'}</span>}
                    </TableCell>
                    <TableCell className='truncate font-mono text-xs' title={c.realm ? `${c.realm}\\${c.username}` : c.username}>
                      {c.realm ? `${c.realm}\\` : ''}
                      {c.username}
                    </TableCell>
                    <TableCell className='truncate font-mono text-xs text-muted-foreground' title={c.secret}>
                      {c.secret || '—'}
                    </TableCell>
                    <TableCell>
                      <Badge variant='outline' className={cn('rounded-sm font-mono text-[10px]', TYPE_COLORS[c.cred_type])}>
                        {TYPE_LABEL[c.cred_type] ?? c.cred_type}
                      </Badge>
                    </TableCell>
                    <TableCell className='truncate text-xs text-muted-foreground' title={c.tag || c.source}>
                      {c.source || c.tag || '—'}
                    </TableCell>
                    <TableCell className='font-mono text-xs text-muted-foreground'>{fmtTime(c.created_at)}</TableCell>
                    <TableCell>
                      <DropdownMenu>
                        <DropdownMenuTrigger asChild>
                          <Button variant='ghost' size='icon' className='size-6'>
                            <Trash2 className='size-3.5 text-muted-foreground' />
                          </Button>
                        </DropdownMenuTrigger>
                        <DropdownMenuContent align='end'>
                          <DropdownMenuItem variant='destructive' onClick={() => del.mutate(c.id)}>
                            <Trash2 className='size-3.5' />
                            删除
                          </DropdownMenuItem>
                        </DropdownMenuContent>
                      </DropdownMenu>
                    </TableCell>
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </div>
      </Main>

      {/* 录入凭据 */}
      <Dialog open={addOpen} onOpenChange={setAddOpen}>
        <DialogContent className='sm:max-w-md'>
          <DialogHeader>
            <DialogTitle>录入凭据</DialogTitle>
            <DialogDescription>手工登记口令 / 哈希（采集结果会自动入库）</DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <div className='grid grid-cols-2 gap-2'>
              <Input placeholder='账户 *' value={form.username} onChange={(e) => setForm({ ...form, username: e.target.value })} />
              <Input placeholder='域 / Realm' value={form.realm} onChange={(e) => setForm({ ...form, realm: e.target.value })} />
            </div>
            <Input placeholder='口令 / 哈希 *' value={form.secret} onChange={(e) => setForm({ ...form, secret: e.target.value })} />
            <div className='grid grid-cols-2 gap-2'>
              <Input placeholder='主机' value={form.host} onChange={(e) => setForm({ ...form, host: e.target.value })} />
              <Select value={form.cred_type} onValueChange={(v) => setForm({ ...form, cred_type: v as Credential['cred_type'] })}>
                <SelectTrigger>
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {(Object.keys(TYPE_LABEL) as Credential['cred_type'][]).map((t) => (
                    <SelectItem key={t} value={t}>
                      {TYPE_LABEL[t]}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
            <Input placeholder='标签(可选)' value={form.tag} onChange={(e) => setForm({ ...form, tag: e.target.value })} />
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setAddOpen(false)}>
              取消
            </Button>
            <Button disabled={!form.username.trim() || !form.secret || add.isPending} onClick={() => add.mutate()}>
              录入
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}
