// 流画面公共容器(真实对接): 桌面(canvas)/摄像头(img)两块画面区, 监控中心面板与音视频监控页共用。
// 数据源: use-media-stream 钩子(deskstream_data / camstream_data)。
import { CameraOff, MonitorPlay } from 'lucide-react'
import { cn } from '@/lib/utils'

export type StreamKind = 'desktop' | 'camera' | 'audio'

export const KIND_LABEL: Record<StreamKind, string> = {
  desktop: '桌面流',
  camera: '摄像头',
  audio: '音频',
}

/**
 * 桌面画面区: 容器撑满, canvas 按设备分辨率等比铺满(纯 CSS, 不写死像素)。
 * hasFrame=false 时显示占位提示, 不显示空黑框。
 */
export function DesktopSurface({
  canvasRef,
  hasFrame,
  hint,
  className,
}: {
  canvasRef: React.RefObject<HTMLCanvasElement | null>
  hasFrame: boolean
  hint: string
  className?: string
}) {
  return (
    <div
      className={cn(
        'relative flex min-h-0 flex-1 items-center justify-center overflow-hidden rounded-lg border bg-[#0a0f14] p-2',
        className
      )}
    >
      <canvas
        ref={canvasRef}
        className='h-full w-auto max-h-full max-w-full'
        style={{ display: hasFrame ? 'block' : 'none' }}
      />
      {!hasFrame && (
        <div className='flex flex-col items-center gap-2 text-muted-foreground'>
          <MonitorPlay className='size-7 opacity-30' />
          <span className='text-xs'>{hint}</span>
        </div>
      )}
    </div>
  )
}

/** 摄像头画面区: 撑满容器等比contain */
export function CameraSurface({
  src,
  hint,
  className,
}: {
  src: string
  hint: string
  className?: string
}) {
  return (
    <div
      className={cn(
        'flex min-h-0 flex-1 items-center justify-center overflow-hidden rounded-lg border bg-[#0a0f14] p-2',
        className
      )}
    >
      {src ? (
        <img
          src={src}
          alt='摄像头画面'
          className='h-full w-auto max-h-full max-w-full object-contain'
          draggable={false}
        />
      ) : (
        <div className='flex flex-col items-center gap-2 text-muted-foreground'>
          <CameraOff className='size-7 opacity-30' />
          <span className='text-xs'>{hint}</span>
        </div>
      )}
    </div>
  )
}
