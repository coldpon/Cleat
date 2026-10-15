// 审计记录(真实对接 ): REST /api/audit(append-only, viewer 及以上可读)。
// 行字段: ts / operator / cmd / mode / session_id / status。按操作员过滤 + 刷新; 上限 200 条。
import { useMemo, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { RefreshCw, ScrollText } from 'lucide-react'
import { cn } from '@/lib/utils'
import { listAudit } from '@/lib/cleat-rest'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

function fmtTime(ts: number): string {
  const d = new Date(ts)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

export function Audit() {
  const [op, setOp] = useState('all')
  const q = useQuery({
    queryKey: ['audit'],
    queryFn: () => listAudit({ limit: 200 }),
    refetchInterval: 15000,
  })
  const items = q.data?.items ?? []
  const total = q.data?.total ?? 0

  const ops = useMemo(() => ['all', ...new Set(items.map((a) => a.operator))], [items])
  const rows = useMemo(() => {
    const filtered = op === 'all' ? items : items.filter((a) => a.operator === op)
    return [...filtered].sort((a, b) => b.ts - a.ts)
  }, [items, op])

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
              <ScrollText className='size-5' />
              审计
            </h2>
            <p className='text-muted-foreground'>
              操作审计流水 · 最近 {items.length} 条 / 总 {total} 条(append-only)
            </p>
          </div>
          <div className='flex flex-wrap items-center gap-1'>
            {ops.map((o) => (
              <button
                key={o}
                onClick={() => setOp(o)}
                className={cn(
                  'rounded-md border px-2.5 py-1 font-mono text-xs transition-colors',
                  op === o
                    ? 'border-border bg-background text-foreground shadow-sm'
                    : 'border-transparent text-muted-foreground hover:bg-background/60'
                )}
              >
                {o === 'all' ? '全部' : o}
              </button>
            ))}
            <Button size='sm' variant='outline' className='h-8 gap-1 text-xs' onClick={() => void q.refetch()}>
              <RefreshCw className={cn('size-3.5', q.isFetching && 'animate-spin')} />
              刷新
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table className='table-fixed'>
            <TableHeader>
              <TableRow>
                <TableHead className='w-44'>时间</TableHead>
                <TableHead className='w-28'>操作员</TableHead>
                <TableHead className='w-44'>动作</TableHead>
                <TableHead className='w-20'>会话</TableHead>
                <TableHead>参数 / 目标</TableHead>
                <TableHead className='w-24'>结果</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {q.isLoading ? (
                <TableRow>
                  <TableCell colSpan={6} className='h-24 text-center text-sm text-muted-foreground'>
                    加载中…
                  </TableCell>
                </TableRow>
              ) : q.isError ? (
                <TableRow>
                  <TableCell colSpan={6} className='h-24 text-center text-sm text-muted-foreground'>
                    加载失败：{(q.error as Error).message}
                    <Button size='sm' variant='outline' className='ms-3 h-7 text-xs' onClick={() => void q.refetch()}>
                      重试
                    </Button>
                  </TableCell>
                </TableRow>
              ) : rows.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={6} className='h-24 text-center text-sm text-muted-foreground'>
                    暂无审计记录
                  </TableCell>
                </TableRow>
              ) : (
                rows.map((a) => {
                  const ok = a.status === 'ok' || a.status === 'success' || a.status === 'completed'
                  return (
                    <TableRow key={a.id} className='h-9'>
                      <TableCell className='font-mono text-xs text-muted-foreground'>{fmtTime(a.ts)}</TableCell>
                      <TableCell className='truncate font-mono text-xs' title={a.operator}>
                        {a.operator}
                      </TableCell>
                      <TableCell className='truncate font-mono text-xs' title={a.cmd}>
                        {a.cmd}
                      </TableCell>
                      <TableCell className='font-mono text-xs text-muted-foreground'>
                        {a.session_id > 0 ? `#${a.session_id}` : '—'}
                      </TableCell>
                      <TableCell className='truncate font-mono text-xs text-muted-foreground' title={a.mode}>
                        {a.mode || '—'}
                      </TableCell>
                      <TableCell>
                        <Badge
                          variant='outline'
                          className={cn(
                            'rounded-sm font-mono text-[10px]',
                            ok ? 'border-emerald-500/40 bg-emerald-500/10 text-emerald-400' : 'border-red-500/40 bg-red-500/10 text-red-400'
                          )}
                        >
                          {a.status}
                        </Badge>
                      </TableCell>
                    </TableRow>
                  )
                })
              )}
            </TableBody>
          </Table>
        </div>
      </Main>
    </>
  )
}
