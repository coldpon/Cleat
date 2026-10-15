// 屏幕截图面板(真实对接 ): ScreenShotSingleShot(可选 quality/scale)。
// TeamServer 把 Agent 的 JPEG 二进制帧转成 data.image(base64)随命令响应返回。
// 墙式展示: 最新在前(上限 12 张控内存); 点击放大(Dialog) + 保存; 挂载自动采集一次。
// 离线会话不发命令(避免 310s 命令超时傻等), 按钮置灰。
import { useCallback, useEffect, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { Camera, Download, Loader2 } from 'lucide-react'
import { toast } from 'sonner'
import { listSessions, sendCommand } from '@/lib/cleat-api'
import { Button } from '@/components/ui/button'
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '@/components/ui/dialog'

interface Shot {
  id: number
  ts: number
  image: string // base64(无 data: 前缀)
  format: string
  quality?: number
  scale?: number
}

const MAX_SHOTS = 12

export function ScreenshotPanel({ sessionId }: { sessionId: number }) {
  const [shots, setShots] = useState<Shot[]>([])
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [zoom, setZoom] = useState<Shot | null>(null)
  const idRef = useRef(0)
  const inFlightRef = useRef(false)

  // 在线状态(离线守卫): 复用共享缓存
  const { data: sessData } = useQuery({
    queryKey: ['sessions'],
    queryFn: listSessions,
    refetchInterval: 3000,
  })
  const online = !!sessData?.sessions.find((s) => s.id === sessionId)?.online

  const capture = useCallback(async () => {
    if (inFlightRef.current) return
    inFlightRef.current = true
    setBusy(true)
    setError('')
    try {
      const resp = await sendCommand<{ image?: string; format?: string; quality?: number; scale?: number }>(
        sessionId,
        'ScreenShotSingleShot'
      )
      const img = resp.data?.image
      if (typeof img !== 'string' || !img) throw new Error('Agent 未返回图像数据')
      const shot: Shot = {
        id: ++idRef.current,
        ts: Date.now(),
        image: img,
        format: resp.data?.format || 'jpeg',
        quality: resp.data?.quality,
        scale: resp.data?.scale,
      }
      setShots((prev) => [shot, ...prev].slice(0, MAX_SHOTS))
    } catch (e) {
      setError((e as Error).message)
      toast.error(`截图失败: ${(e as Error).message}`)
    } finally {
      setBusy(false)
      inFlightRef.current = false
    }
  }, [sessionId])

  // 挂载自动采集一次(点开即有图; 离线等上线后由用户手动触发)
  useEffect(() => {
    if (sessData === undefined) return // 列表未就绪
    if (online && shots.length === 0 && !busy && !error) void capture()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sessData, online])

  const dataUrl = (s: Shot) => `data:image/${s.format};base64,${s.image}`

  const save = (s: Shot) => {
    const a = document.createElement('a')
    a.href = dataUrl(s)
    a.download = `snapshot-${sessionId}-${s.ts}.${s.format === 'png' ? 'png' : 'jpg'}`
    a.click()
  }

  return (
    <div className='flex h-full select-none flex-col overflow-y-auto p-4'>
      <div className='mb-4 flex items-center justify-between'>
        <div>
          <h3 className='text-sm font-medium'>屏幕截图</h3>
          <p className='text-xs text-muted-foreground'>
            会话 #{sessionId} · 共 {shots.length} 张{shots.length >= MAX_SHOTS ? `（仅保留最近 ${MAX_SHOTS} 张）` : ''}
          </p>
        </div>
        <Button size='sm' className='h-7 gap-1 text-xs' disabled={busy || !online} onClick={() => void capture()}>
          {busy ? <Loader2 className='size-3.5 animate-spin' /> : <Camera className='size-3.5' />}
          {busy ? '采集中…' : '截图'}
        </Button>
      </div>

      {error && (
        <p className='mb-3 rounded-md border border-red-500/40 bg-red-500/10 px-3 py-2 text-xs text-red-400'>
          {error}
        </p>
      )}

      {shots.length === 0 && !busy && !error ? (
        <div className='flex flex-1 flex-col items-center justify-center gap-1 text-sm text-muted-foreground'>
          <Camera className='size-6 opacity-30' />
          {online ? '暂无截图 · 点击右上角"截图"捕获远程屏幕' : '会话离线，上线后可采集'}
        </div>
      ) : (
        <div className='grid grid-cols-2 gap-3 sm:grid-cols-3 lg:grid-cols-4'>
          {shots.map((s) => (
            <div
              key={s.id}
              className='group cursor-zoom-in overflow-hidden rounded-lg border transition-shadow hover:shadow-md'
              onClick={() => setZoom(s)}
            >
              <img src={dataUrl(s)} alt={`截图 #${s.id}`} className='w-full' loading='lazy' />
              <div className='flex items-center justify-between px-2 py-1.5 text-[10px] text-muted-foreground'>
                <span className='font-mono'>#{s.id} · {new Date(s.ts).toLocaleTimeString()}</span>
                <span className='flex items-center gap-1'>
                  {s.scale != null && s.scale !== 100 && <span>{s.scale}%</span>}
                  <button
                    title='保存'
                    className='rounded p-0.5 opacity-0 transition-opacity hover:bg-muted hover:text-foreground group-hover:opacity-100'
                    onClick={(e) => {
                      e.stopPropagation()
                      save(s)
                    }}
                  >
                    <Download className='size-3' />
                  </button>
                </span>
              </div>
            </div>
          ))}
        </div>
      )}

      {/* 大图查看 */}
      <Dialog open={zoom !== null} onOpenChange={(o) => !o && setZoom(null)}>
        <DialogContent className='max-w-5xl gap-0 overflow-hidden p-0'>
          <DialogHeader className='flex-row items-center gap-2 space-y-0 border-b px-4 py-3'>
            <DialogTitle className='flex items-center gap-2 text-sm font-medium'>
              <Camera className='size-4 text-muted-foreground' />
              截图 · {zoom ? new Date(zoom.ts).toLocaleString() : ''}
            </DialogTitle>
            <span className='ms-auto font-mono text-[10px] text-muted-foreground'>
              {zoom ? `#${zoom.id} · ${zoom.format}${zoom.quality ? ` · Q${zoom.quality}` : ''}${zoom.scale ? ` · ${zoom.scale}%` : ''}` : ''}
            </span>
            <Button
              size='sm'
              variant='outline'
              className='h-7 gap-1 text-xs'
              onClick={() => {
                if (zoom) save(zoom)
              }}
            >
              <Download className='size-3.5' />
              保存
            </Button>
          </DialogHeader>
          {zoom && <img src={dataUrl(zoom)} alt='截图放大' className='w-full' />}
        </DialogContent>
      </Dialog>
    </div>
  )
}
