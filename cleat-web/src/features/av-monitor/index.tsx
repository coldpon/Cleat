// 音视频监控页(作战导航 · 真实对接 ): 两级结构 —— 监控墙(全部在线会话) → 单路大画面。
// 桌面/摄像头/音频 是同一会话的三个"镜头", 同页切换, 不拆三个页面。
// 选中态走 URL search 参数(/av-monitor?session=&kind=): 会话右键菜单直接跳入, 刷新不丢。
// 数据流: 大画面内「开始」下发 DesktopStreamStart / CameraStreamStart / AudioStartRecord →
//        会话级 SSE 帧回推(deskstream_data / camstream_data / audio_data) 实时渲染。
// 生命周期: 离开大画面(返回监控墙/切会话/切镜头/关页)自动停掉本条流 —— 不遗留会话侧采集。
import { useCallback, useEffect, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { ArrowLeft, Expand, Mic, MonitorPlay, Video } from 'lucide-react'
import { toast } from 'sonner'
import { listSessions, type SessionRow } from '@/lib/cleat-api'
import {
  startAudioRecording,
  startCameraStream,
  startDesktopStream,
  stopAudioRecording,
  stopCameraStream,
  stopDesktopStream,
} from '@/lib/cleat-monitor'
import { isLinuxOs } from '@/lib/platform'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search as GlobalSearch } from '@/components/search'
import { KIND_LABEL, DesktopSurface, CameraSurface, type StreamKind } from '@/features/stream/stream-content'
import { useMediaStream } from '@/features/stream/use-media-stream'

const KINDS: StreamKind[] = ['desktop', 'camera', 'audio']

function startByKind(id: number, kind: StreamKind, desktopInterval: number, cameraInterval: number) {
  if (kind === 'desktop') return startDesktopStream(id, { interval: desktopInterval })
  if (kind === 'camera') return startCameraStream(id, { interval: cameraInterval })
  return startAudioRecording(id)
}

function stopByKind(id: number, kind: StreamKind) {
  if (kind === 'desktop') return stopDesktopStream(id)
  if (kind === 'camera') return stopCameraStream(id)
  return stopAudioRecording(id)
}

export function AvMonitor({ sessionId, kind }: { sessionId?: number; kind?: StreamKind }) {
  const navigate = useNavigate()
  const { data } = useQuery({ queryKey: ['sessions'], queryFn: listSessions, refetchInterval: 5000 })
  const sessions = data?.sessions

  const openStream = (id: number, k: StreamKind) =>
    navigate({ to: '/av-monitor', search: { session: id, kind: k } })
  const backToWall = () => navigate({ to: '/av-monitor', search: {} })

  const list = sessions ?? []
  const online = list.filter((s) => s.online)
  const offline = list.length - online.length
  const selected = sessionId != null ? list.find((s) => s.id === sessionId) : undefined

  return (
    <>
      <Header fixed>
        <GlobalSearch className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main fluid className='flex h-[calc(100svh-4rem)] flex-col overflow-hidden p-4'>
        {sessionId != null && sessions === undefined ? (
          <div className='flex flex-1 items-center justify-center text-sm text-muted-foreground'>加载中…</div>
        ) : selected ? (
          <BigView
            key={`${selected.id}:${kind ?? 'desktop'}`}
            session={selected}
            kind={kind ?? 'desktop'}
            onBack={backToWall}
            onKind={(k) => openStream(selected.id, k)}
          />
        ) : (
          <WallView online={online} offline={offline} loading={sessions === undefined} onOpen={(id) => openStream(id, 'desktop')} />
        )}
      </Main>
    </>
  )
}

// ---------- 一级: 监控墙(全部在线会话) ----------
function WallView({
  online,
  offline,
  loading,
  onOpen,
}: {
  online: SessionRow[]
  offline: number
  loading: boolean
  onOpen: (id: number) => void
}) {
  return (
    <>
      <div className='mb-3 flex flex-wrap items-end justify-between gap-2'>
        <div>
          <h2 className='flex items-center gap-2 text-2xl font-bold tracking-tight'>
            <Video className='size-5' />
            音视频监控
          </h2>
          <p className='text-muted-foreground'>
            在线会话 <span className='font-medium text-emerald-500'>{online.length}</span> 个
            {offline > 0 && (
              <>
                {' '}
                · 离线 <span className='font-medium'>{offline}</span> 个（不显示）
              </>
            )}{' '}
            · 点击画面进入大画面开启监控流
          </p>
        </div>
      </div>

      <div className='min-h-0 flex-1 overflow-y-auto pb-1'>
        {loading ? (
          <div className='flex h-full items-center justify-center text-sm text-muted-foreground'>加载中…</div>
        ) : online.length === 0 ? (
          <div className='flex h-96 flex-col items-center justify-center gap-2 rounded-lg border border-dashed text-sm text-muted-foreground'>
            <Video className='size-8 opacity-30' />
            暂无在线会话
          </div>
        ) : (
          <div className='grid gap-3 sm:grid-cols-2 xl:grid-cols-3'>
            {online.map((s) => (
              <StreamCard key={s.id} session={s} onOpen={onOpen} />
            ))}
          </div>
        )}
      </div>
    </>
  )
}

function StreamCard({ session, onOpen }: { session: SessionRow; onOpen: (id: number) => void }) {
  return (
    <div className='overflow-hidden rounded-lg border bg-card'>
      {/* 卡片头: 会话标识 */}
      <div className='flex items-center gap-2 px-2.5 py-1.5'>
        <span className='size-2 shrink-0 rounded-full bg-emerald-500' />
        <span className='truncate font-mono text-xs font-medium'>{session.hostname}</span>
        <span className='truncate text-[11px] text-muted-foreground'>{session.local_ip}</span>
        <Badge variant='secondary' className='ms-auto shrink-0 rounded-sm font-mono text-[10px] uppercase'>
          {session.transport || '—'}
        </Badge>
      </div>

      {/* 画面占位(流在大画面内手动开启, 墙面本身不占用会话带宽) */}
      <button
        onClick={() => onOpen(session.id)}
        title='进入大画面'
        className='group/preview relative block aspect-video w-full overflow-hidden border-y bg-[#09090b]'
      >
        <div className='flex h-full w-full items-center justify-center'>
          <MonitorPlay className='size-8 text-zinc-700' />
        </div>
        <span className='absolute inset-0 flex items-center justify-center gap-2 bg-black/0 text-sm font-medium text-white opacity-0 transition-all group-hover/preview:bg-black/40 group-hover/preview:opacity-100'>
          <Expand className='size-4' />
          进入大画面
        </span>
      </button>

      {/* 卡片脚: 基础信息 */}
      <div className='flex items-center gap-3 px-2.5 py-1.5 text-[10px] text-muted-foreground'>
        <span className='font-mono'>#{session.id}</span>
        <span className='truncate'>{session.privilege || '—'}</span>
        <span className='ms-auto shrink-0'>需在大画面内开启监控流</span>
      </div>
    </div>
  )
}

// ---------- 二级: 单路大画面(占满内容区, 非弹层) ----------
function BigView({
  session,
  kind,
  onBack,
  onKind,
}: {
  session: SessionRow
  kind: StreamKind
  onBack: () => void
  onKind: (k: StreamKind) => void
}) {
  const canWrite = useCanWrite()
  const linux = isLinuxOs(session.os || '')
  const [running, setRunning] = useState(false)
  const [busy, setBusy] = useState(false)
  const [desktopInterval, setDesktopInterval] = useState(500)
  const [cameraInterval, setCameraInterval] = useState(1000)
  const runningRef = useRef(false)
  const media = useMediaStream(session.id)

  // 离开大画面(返回/切镜头/切会话/关页): 停掉本条流 —— 不遗留会话侧采集
  useEffect(() => {
    return () => {
      if (runningRef.current) {
        void stopByKind(session.id, kind).catch(() => undefined)
        runningRef.current = false
      }
    }
  }, [session.id, kind])

  const platformBlocked = linux && (kind === 'camera' || kind === 'audio')

  const toggle = useCallback(async () => {
    if (!canWrite) {
      toast.warning('当前角色无监控操作权限')
      return
    }
    if (busy || platformBlocked) return
    setBusy(true)
    try {
      if (running) {
        await stopByKind(session.id, kind)
        runningRef.current = false
        setRunning(false)
        toast.success(`${KIND_LABEL[kind]}已停止`)
      } else {
        await startByKind(session.id, kind, desktopInterval, cameraInterval)
        runningRef.current = true
        setRunning(true)
        toast.success(`${KIND_LABEL[kind]}已开始`)
      }
    } catch (e) {
      toast.error(`${KIND_LABEL[kind]}${running ? '停止' : '开始'}失败: ${(e as Error).message}`)
    } finally {
      setBusy(false)
    }
  }, [busy, cameraInterval, canWrite, desktopInterval, kind, platformBlocked, running, session.id])

  const statusText =
    kind === 'desktop' ? media.desktopInfo : kind === 'camera' ? media.cameraInfo : media.audioStatus

  return (
    <>
      {/* 工具条 */}
      <div className='mb-3 flex flex-wrap items-center gap-2'>
        <Button variant='outline' size='sm' className='h-8 gap-1.5' onClick={onBack}>
          <ArrowLeft className='size-4' />
          返回监控墙
        </Button>
        <span className='flex items-center gap-2 text-sm font-medium'>
          <span className={cn('size-2 rounded-full', session.online ? 'bg-emerald-500' : 'bg-muted-foreground/40')} />
          {KIND_LABEL[kind]} · {session.hostname}
        </span>
        <span className='hidden font-mono text-xs text-muted-foreground sm:inline'>
          {session.local_ip}
          {statusText && statusText !== '未连接' ? ` · ${statusText}` : ''}
        </span>
        {/* 流类型切换(同一会话的三个镜头) */}
        <div className='ms-auto flex items-center gap-0.5 rounded-md border bg-muted/40 p-0.5'>
          {KINDS.map((k) => {
            const blocked = linux && (k === 'camera' || k === 'audio')
            return (
              <button
                key={k}
                onClick={() => !blocked && onKind(k)}
                disabled={blocked}
                title={blocked ? 'Linux 会话不支持该采集' : undefined}
                className={cn(
                  'rounded px-2 py-0.5 text-xs transition-colors',
                  kind === k
                    ? 'bg-background text-foreground shadow-sm'
                    : 'text-muted-foreground hover:text-foreground',
                  blocked && 'cursor-not-allowed opacity-40 hover:text-muted-foreground'
                )}
              >
                {KIND_LABEL[k]}
              </button>
            )
          })}
        </div>
        {/* 间隔(仅桌面/摄像头) */}
        {kind !== 'audio' && (
          <div className='flex items-center gap-1.5'>
            <span className='text-xs text-muted-foreground'>间隔</span>
            <Input
              type='number'
              min={100}
              max={60000}
              value={kind === 'desktop' ? desktopInterval : cameraInterval}
              onChange={(e) => {
                const v = Number(e.target.value) || 500
                if (kind === 'desktop') setDesktopInterval(v)
                else setCameraInterval(v)
              }}
              disabled={running}
              className='h-8 w-24 text-xs'
            />
            <span className='text-xs text-muted-foreground'>ms</span>
          </div>
        )}
        <Button
          size='sm'
          variant={running ? 'destructive' : 'default'}
          className='h-8'
          disabled={busy || platformBlocked || !session.online || (!running && !canWrite)}
          onClick={() => void toggle()}
        >
          {busy ? '…' : running ? '停止' : '开始'}
        </Button>
      </div>

      {/* 大画面: 占满剩余高度 */}
      <div className='flex min-h-0 flex-1 flex-col'>
        {platformBlocked ? (
          <div className='flex flex-1 items-center justify-center rounded-lg border border-dashed text-sm text-muted-foreground'>
            Linux 会话不支持{KIND_LABEL[kind]}采集 —— 返回监控墙选择其他会话
          </div>
        ) : kind === 'desktop' ? (
          <DesktopSurface
            canvasRef={media.desktopCanvasRef}
            hasFrame={media.desktopHasFrame}
            hint={
              !session.online
                ? '会话离线'
                : canWrite
                  ? '点击「开始」后此处显示远程桌面画面'
                  : '当前角色无监控操作权限'
            }
            className='flex-1'
          />
        ) : kind === 'camera' ? (
          <CameraSurface
            src={media.cameraImg}
            hint={
              !session.online ? '会话离线' : canWrite ? '点击「开始」后此处显示摄像头画面' : '当前角色无监控操作权限'
            }
            className='flex-1'
          />
        ) : (
          /* 音频: 波形 + 保存 WAV */
          <div className='flex flex-1 flex-col items-center justify-center gap-4 rounded-lg border bg-[#09090b]'>
            <div className='flex h-24 items-end gap-1 px-10'>
              {Array.from({ length: 48 }).map((_, i) => (
                <span
                  key={i}
                  className={cn(
                    'w-1.5 rounded-sm transition-all duration-300',
                    running ? 'bg-emerald-500' : 'bg-slate-700'
                  )}
                  style={{ height: running ? `${20 + ((i * 37) % 80)}%` : '4%' }}
                />
              ))}
            </div>
            <div className='flex items-center gap-2'>
              <Mic className='size-4 text-muted-foreground' />
              <span className='text-sm text-muted-foreground'>{media.audioStatus}</span>
            </div>
            <Button
              size='sm'
              variant='outline'
              className='h-8 text-xs'
              disabled={media.audioBytes === 0}
              onClick={() => {
                const n = media.saveWav()
                if (n > 0) toast.success(`已保存录音 (${(n / 1024).toFixed(0)} KB)`)
                else toast.warning('没有录音数据')
              }}
            >
              保存 WAV（已缓冲 {(media.audioBytes / 1024).toFixed(0)} KB）
            </Button>
            <p className='text-xs text-muted-foreground'>PCM 音频经 SSE 推送，边收边播；停止后可导出 WAV</p>
          </div>
        )}
      </div>
    </>
  )
}
