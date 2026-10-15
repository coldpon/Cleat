// 载荷生成(真实对接 ): REST /api/payloads(generate/list/download/delete)。
//   POST /api/payloads/generate  模板烧录生成样本(同步毫秒级, 配置烧入配置块, 零参数运行)
//   GET  /api/payloads           已生成样本清单
//   GET  /api/payloads/{n}/download  认证下载(Bearer)
//   DELETE /api/payloads/{n}     删除样本
// 传输参数按协议映射: dns → dns_server/dns_domain; wss → wss_url; icmp → icmp_target。
import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { ChevronDown, Copy, Download, Package, Sparkles, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { cn } from '@/lib/utils'
import {
  deletePayload,
  downloadPayload,
  generatePayload,
  listPayloadStubs,
  listPayloads,
  type PayloadResult,
} from '@/lib/cleat-rest'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from '@/components/ui/collapsible'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

const FORMS = [
  { value: 'exe', label: 'EXE', desc: '直接运行' },
  { value: 'dll', label: 'DLL', desc: 'rundll32 加载' },
  { value: 'shc', label: 'SHC', desc: '裸字节 · 注入用' },
]
const TRANSPORTS = [
  { value: 'tcp', label: 'TCP', desc: '明文/加密隧道' },
  { value: 'https', label: 'HTTPS', desc: 'Beacon 通道' },
  { value: 'wss', label: 'WSS', desc: 'WebSocket' },
  { value: 'dns', label: 'DNS', desc: 'DNS 隧道' },
  { value: 'icmp', label: 'ICMP', desc: 'ICMP 隧道' },
]

function fmtSize(n: number): string {
  if (!n || n <= 0) return '—'
  const k = 1024
  const sizes = ['B', 'KB', 'MB']
  const i = Math.min(sizes.length - 1, Math.floor(Math.log(n) / Math.log(k)))
  return `${(n / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`
}

function fmtTime(ts: string): string {
  if (!ts) return '—'
  return ts.replace('T', ' ').slice(0, 19)
}

export function Payloads() {
  const qc = useQueryClient()
  const [name, setName] = useState('')
  const [form, setForm] = useState('exe')
  const [transport, setTransport] = useState('tcp')
  const [host, setHost] = useState('127.0.0.1')
  const [port, setPort] = useState('4444')
  const [keyMode, setKeyMode] = useState('x25519')
  const [psk, setPsk] = useState('')
  const [dnsServer, setDnsServer] = useState('')
  const [domain, setDomain] = useState('')
  const [advancedOpen, setAdvancedOpen] = useState(false)
  const [result, setResult] = useState<PayloadResult | null>(null)

  // 已生成样本清单
  const listQ = useQuery({ queryKey: ['payloads'], queryFn: listPayloads })
  const entries = listQ.data?.payloads ?? []

  const invalidate = () => void qc.invalidateQueries({ queryKey: ['payloads'] })

  const generate = useMutation({
    mutationFn: () => {
      const req: Record<string, unknown> = {
        form,
        transport,
        host: host.trim(),
        port: port.trim(),
        key_mode: keyMode,
        ...(name.trim() ? { name: name.trim() } : {}),
        ...(keyMode === 'psk' ? { psk: psk.trim() } : {}),
        ...(transport === 'dns'
          ? { dns_server: dnsServer.trim(), dns_domain: domain.trim() }
          : {}),
        ...(transport === 'wss' ? { wss_url: `wss://${host.trim()}:${port.trim()}` } : {}),
        ...(transport === 'icmp' ? { icmp_target: host.trim() } : {}),
      }
      return generatePayload(req)
    },
    onSuccess: (r) => {
      setResult(r)
      toast.success(`样本生成成功：${r.name}`)
      if (r.warnings?.length) toast.warning(r.warnings.join('\n'))
      invalidate()
    },
    onError: (e) => toast.error(`生成失败: ${(e as Error).message}`),
  })

  const del = useMutation({
    mutationFn: deletePayload,
    onSuccess: () => {
      toast.success('样本已删除')
      invalidate()
    },
    onError: (e) => toast.error(`删除失败: ${(e as Error).message}`),
  })

  const doGenerate = () => {
    if (!host.trim()) {
      toast.error('请填写 C2 主机地址')
      return
    }
    if (keyMode === 'psk' && !psk.trim()) {
      toast.error('PSK 模式需要填写密钥')
      return
    }
    generate.mutate()
  }

  const stamp = () =>
    new Date().toISOString().slice(0, 10).replace(/-/g, '') + '-' + new Date().toTimeString().slice(0, 8).replace(/:/g, '')

  // 内置 stub 元数据(信息条: 提交/版本/大小 —— 防"烧录了落后 stub 缺特性"的沉默坑)
  const stubsQ = useQuery({ queryKey: ['payload-stubs'], queryFn: listPayloadStubs, staleTime: 60_000 })
  const exeStub = stubsQ.data?.stubs.find((s) => s.form === 'exe')

  return (
    <>
      <Header fixed>
        <Search className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main>
        <div className='mb-4'>
          <h2 className='flex items-center gap-2 text-2xl font-bold tracking-tight'>
            <Package className='size-5' />
            载荷生成
          </h2>
          <p className='text-muted-foreground'>生成 exe / dll / shc 样本 · 配置烧入配置块 · 零参数直接运行</p>
        </div>

        {/* 内置 stub 信息条 */}
        {exeStub && (
          <div className='mb-4 flex flex-wrap items-center gap-x-2 gap-y-1 rounded-md border border-sky-500/30 bg-sky-500/5 px-3 py-2 text-xs'>
            <span className='text-muted-foreground'>内置 stub:</span>
            <code className='font-mono'>{exeStub.form}</code>
            <code className='font-mono text-muted-foreground'>{exeStub.agent_commit || '未知提交'}</code>
            <span className='text-muted-foreground'>
              · block v{exeStub.block_version} · {fmtSize(exeStub.size)} · {exeStub.built_at || '未标注构建时间'}
            </span>
            <span className='ms-auto text-muted-foreground'>落后于当前 Agent 提交时可用 -stub-dir 覆盖为最新 stub</span>
          </div>
        )}

        <div className='grid gap-4 lg:grid-cols-2'>
          {/* 左: 表单 */}
          <div className='space-y-4'>
            <div className='rounded-lg border p-4'>
              <Label className='text-sm font-medium'>形态</Label>
              <div className='mt-2 grid grid-cols-3 gap-2'>
                {FORMS.map((f) => (
                  <button
                    key={f.value}
                    onClick={() => setForm(f.value)}
                    className={cn(
                      'rounded-md border p-2.5 text-start transition-colors',
                      form === f.value ? 'border-primary bg-primary/5' : 'hover:bg-accent/50'
                    )}
                  >
                    <p className='font-mono text-sm font-medium'>{f.label}</p>
                    <p className='text-xs text-muted-foreground'>{f.desc}</p>
                  </button>
                ))}
              </div>
            </div>

            <div className='rounded-lg border p-4'>
              <Label className='text-sm font-medium'>传输协议</Label>
              <div className='mt-2 flex flex-wrap gap-2'>
                {TRANSPORTS.map((t) => (
                  <button
                    key={t.value}
                    onClick={() => setTransport(t.value)}
                    className={cn(
                      'rounded-md border px-3 py-1.5 text-sm transition-colors',
                      transport === t.value ? 'border-primary bg-primary/5 font-medium' : 'hover:bg-accent/50'
                    )}
                  >
                    {t.label}
                  </button>
                ))}
              </div>
            </div>

            <div className='space-y-3 rounded-lg border p-4'>
              <div className='grid grid-cols-3 gap-3'>
                <div className='col-span-2 space-y-1.5'>
                  <Label htmlFor='host'>C2 主机</Label>
                  <Input id='host' value={host} onChange={(e) => setHost(e.target.value)} placeholder='1.2.3.4' />
                </div>
                <div className='space-y-1.5'>
                  <Label htmlFor='port'>端口</Label>
                  <Input id='port' value={port} onChange={(e) => setPort(e.target.value)} placeholder='4444' />
                </div>
              </div>

              <div className='space-y-1.5'>
                <Label htmlFor='pname'>产物名（可选）</Label>
                <Input id='pname' value={name} onChange={(e) => setName(e.target.value)} placeholder={`${transport}-${stamp()}`} />
              </div>

              <div className='space-y-1.5'>
                <Label>密钥模式</Label>
                <div className='flex gap-2'>
                  <button
                    onClick={() => setKeyMode('x25519')}
                    className={cn(
                      'flex-1 rounded-md border p-2 text-start text-sm transition-colors',
                      keyMode === 'x25519' ? 'border-primary bg-primary/5 font-medium' : 'hover:bg-accent/50'
                    )}
                  >
                    X25519
                    <span className='ms-1.5 text-xs text-muted-foreground'>样本内只留公钥（推荐）</span>
                  </button>
                  <button
                    onClick={() => setKeyMode('psk')}
                    className={cn(
                      'flex-1 rounded-md border p-2 text-start text-sm transition-colors',
                      keyMode === 'psk' ? 'border-primary bg-primary/5 font-medium' : 'hover:bg-accent/50'
                    )}
                  >
                    PSK
                    <span className='ms-1.5 text-xs text-muted-foreground'>仅 dev 用</span>
                  </button>
                </div>
              </div>

              {keyMode === 'psk' && (
                <div className='space-y-1.5'>
                  <Label htmlFor='psk'>PSK（64 位 hex）</Label>
                  <Input id='psk' value={psk} onChange={(e) => setPsk(e.target.value)} className='font-mono' placeholder='cd7190d1…' />
                </div>
              )}

              {(transport === 'dns' || transport === 'wss' || transport === 'icmp') && (
                <Collapsible open={advancedOpen} onOpenChange={setAdvancedOpen}>
                  <CollapsibleTrigger className='flex w-full items-center gap-1 text-sm text-muted-foreground hover:text-foreground'>
                    <ChevronDown className={cn('size-4 transition-transform', advancedOpen && 'rotate-180')} />
                    高级选项（DNS / 通道参数）
                  </CollapsibleTrigger>
                  <CollapsibleContent className='space-y-3 pt-3'>
                    {transport === 'dns' && (
                      <div className='grid grid-cols-2 gap-3'>
                        <div className='space-y-1.5'>
                          <Label htmlFor='dns'>DNS Server</Label>
                          <Input id='dns' value={dnsServer} onChange={(e) => setDnsServer(e.target.value)} placeholder='8.8.8.8:53' />
                        </div>
                        <div className='space-y-1.5'>
                          <Label htmlFor='dom'>查询域名</Label>
                          <Input id='dom' value={domain} onChange={(e) => setDomain(e.target.value)} placeholder='cdn.example.com' />
                        </div>
                      </div>
                    )}
                    {transport === 'wss' && (
                      <p className='text-xs text-muted-foreground'>WSS 地址自动组为 wss://&#123;C2 主机&#125;:&#123;端口&#125;（默认路径 /ws）</p>
                    )}
                    {transport === 'icmp' && <p className='text-xs text-muted-foreground'>ICMP 隧道目标即上方 C2 主机</p>}
                  </CollapsibleContent>
                </Collapsible>
              )}

              <Button className='w-full' onClick={doGenerate} disabled={generate.isPending}>
                {generate.isPending ? (
                  <>
                    <Sparkles className='size-4 animate-pulse' />
                    生成中…
                  </>
                ) : (
                  <>
                    <Sparkles className='size-4' />
                    生成样本
                  </>
                )}
              </Button>
            </div>
          </div>

          {/* 右: 生成结果 */}
          <div>
            {!result && !generate.isPending && (
              <div className='flex h-full min-h-64 flex-col items-center justify-center gap-2 rounded-lg border border-dashed text-muted-foreground'>
                <Package className='size-8 opacity-30' />
                <p className='text-sm'>填写左侧表单后点击「生成样本」</p>
                <p className='text-xs opacity-70'>模板烧录 · 同步毫秒级返回</p>
              </div>
            )}
            {generate.isPending && (
              <div className='flex h-full min-h-64 flex-col items-center justify-center gap-3 rounded-lg border border-dashed'>
                <Sparkles className='size-8 animate-pulse text-primary' />
                <p className='text-sm text-muted-foreground'>正在生成样本…</p>
              </div>
            )}
            {result && (
              <div className='rounded-lg border p-4'>
                <div className='mb-3 flex items-center gap-2'>
                  <Badge className='bg-emerald-600'>✓ 生成成功</Badge>
                  <span className='text-xs text-muted-foreground'>{fmtTime(result.built_at)}</span>
                </div>
                <dl className='space-y-2.5 text-sm'>
                  <div>
                    <dt className='text-xs text-muted-foreground'>名称</dt>
                    <dd className='font-mono'>{result.name}</dd>
                  </div>
                  <div>
                    <dt className='text-xs text-muted-foreground'>形态</dt>
                    <dd className='font-mono uppercase'>{result.form}</dd>
                  </div>
                  <div>
                    <dt className='text-xs text-muted-foreground'>产物路径</dt>
                    <dd className='break-all font-mono text-xs'>{result.bin_path}</dd>
                  </div>
                  <div>
                    <dt className='text-xs text-muted-foreground'>运行方式</dt>
                    <dd className='font-mono text-xs'>
                      {result.run_line} <span className='text-muted-foreground'>（配置已烧入，零参数运行）</span>
                    </dd>
                  </div>
                </dl>
                {result.warnings && result.warnings.length > 0 && (
                  <p className='mt-2 rounded-md border border-amber-500/40 bg-amber-500/10 px-2 py-1.5 text-xs text-amber-500'>
                    {result.warnings.join('；')}
                  </p>
                )}
                <div className='mt-4 flex gap-2'>
                  <Button size='sm' onClick={() => void downloadPayload(result.name)}>
                    <Download className='size-3.5' />
                    下载
                  </Button>
                  <Button
                    size='sm'
                    variant='outline'
                    onClick={() => {
                      void navigator.clipboard.writeText(result.run_line)
                      toast.success('已复制运行命令')
                    }}
                  >
                    <Copy className='size-3.5' />
                    复制运行命令
                  </Button>
                </div>
              </div>
            )}
          </div>
        </div>

        {/* 已生成样本清单 */}
        <div className='mt-6'>
          <h3 className='mb-2 text-sm font-medium'>已生成样本</h3>
          <div className='overflow-hidden rounded-md border'>
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead className='w-56'>名称</TableHead>
                  <TableHead className='w-20'>形态</TableHead>
                  <TableHead className='w-28'>大小</TableHead>
                  <TableHead>生成时间</TableHead>
                  <TableHead className='w-32 text-end'>操作</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {listQ.isLoading ? (
                  <TableRow>
                    <TableCell colSpan={5} className='h-16 text-center text-sm text-muted-foreground'>
                      加载中…
                    </TableCell>
                  </TableRow>
                ) : entries.length === 0 ? (
                  <TableRow>
                    <TableCell colSpan={5} className='h-16 text-center text-sm text-muted-foreground'>
                      还没有生成过样本
                    </TableCell>
                  </TableRow>
                ) : (
                  entries.map((p) => (
                    <TableRow key={p.name} className='h-9'>
                      <TableCell className='truncate font-mono text-xs' title={p.name}>
                        {p.name}
                      </TableCell>
                      <TableCell className='font-mono text-xs uppercase'>{p.form}</TableCell>
                      <TableCell className='font-mono text-xs text-muted-foreground'>{fmtSize(p.size)}</TableCell>
                      <TableCell className='font-mono text-xs text-muted-foreground'>{fmtTime(p.built_at)}</TableCell>
                      <TableCell className='text-end'>
                        <div className='flex justify-end gap-1'>
                          <Button
                            size='sm'
                            variant='ghost'
                            className='h-7 gap-1 text-xs'
                            onClick={() => void downloadPayload(p.name)}
                          >
                            <Download className='size-3.5' />
                            下载
                          </Button>
                          <Button
                            size='sm'
                            variant='ghost'
                            className='h-6 w-6 p-0 text-muted-foreground hover:text-red-400'
                            title='删除'
                            onClick={() => {
                              if (window.confirm(`删除样本 ${p.name}？`)) del.mutate(p.name)
                            }}
                          >
                            <Trash2 className='size-3.5' />
                          </Button>
                        </div>
                      </TableCell>
                    </TableRow>
                  ))
                )}
              </TableBody>
            </Table>
          </div>
        </div>
      </Main>
    </>
  )
}
