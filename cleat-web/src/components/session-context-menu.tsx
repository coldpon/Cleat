// 会话行右键菜单(FlyC2/CS 式):
//   工作区功能(开标签): 交互终端 / 文件管理 / 进程管理 / 主机信息 / 监控中心 / 屏幕截图 / 运行时配置
//   媒体流(音视频监控页): 桌面流 / 摄像头 / 音频 —— 应用内切页进大画面, 不再弹浏览器窗口。
import { useNavigate } from '@tanstack/react-router'
import {
  Activity,
  Camera,
  FolderTree,
  ListTree,
  Mic,
  Monitor,
  Settings,
  SquareTerminal,
  Terminal,
  Video,
} from 'lucide-react'
import { sessionFnTab, useTabsStore, type TabKind } from '@/stores/tabs-store'

const WORKSPACE_ITEMS: { kind: TabKind; label: string; icon: React.ElementType }[] = [
  { kind: 'terminal', label: '交互终端', icon: Terminal },
  { kind: 'files', label: '文件管理', icon: FolderTree },
  { kind: 'processes', label: '主机管理', icon: ListTree },
  { kind: 'hostinfo', label: '主机信息', icon: Monitor },
  { kind: 'monitor', label: '监控中心', icon: Activity },
  { kind: 'screenshot', label: '屏幕截图', icon: Camera },
  { kind: 'commands', label: '命令面板', icon: SquareTerminal },
  { kind: 'config', label: '运行时配置', icon: Settings },
]

const WINDOW_ITEMS: { kind: 'desktop' | 'camera' | 'audio'; label: string; icon: React.ElementType }[] = [
  { kind: 'desktop', label: '桌面流', icon: Monitor },
  { kind: 'camera', label: '摄像头', icon: Video },
  { kind: 'audio', label: '音频', icon: Mic },
]

export function SessionContextMenu({
  x,
  y,
  hostname,
  sessionId,
  onClose,
}: {
  x: number
  y: number
  hostname: string
  sessionId: number
  onClose: () => void
}) {
  const openTab = useTabsStore((s) => s.openTab)
  const navigate = useNavigate()

  // 边界防溢出(菜单更高: 6 工作区 + 分隔 + 3 媒体)
  const left = Math.min(x, window.innerWidth - 200)
  const top = Math.min(y, window.innerHeight - 300)

  return (
    <>
      <div className='fixed inset-0 z-50' onClick={onClose} onContextMenu={(e) => { e.preventDefault(); onClose() }} />
      <div
        className='fixed z-50 min-w-44 rounded-md border bg-popover p-1 text-popover-foreground shadow-md'
        style={{ left, top }}
      >
        {WORKSPACE_ITEMS.map((it) => (
          <button
            key={it.kind}
            className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
            onClick={() => {
              openTab(sessionFnTab(sessionId, hostname, it.kind))
              onClose()
            }}
          >
            <it.icon className='size-3.5 text-muted-foreground' />
            {it.label}
          </button>
        ))}

        <div className='my-1 border-t' />
        <p className='px-2 py-0.5 text-[10px] uppercase tracking-wide text-muted-foreground'>音视频监控</p>
        {WINDOW_ITEMS.map((it) => (
          <button
            key={it.kind}
            className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
            onClick={() => {
              navigate({ to: '/av-monitor', search: { session: sessionId, kind: it.kind } })
              onClose()
            }}
          >
            <it.icon className='size-3.5 text-muted-foreground' />
            {it.label}
          </button>
        ))}
      </div>
    </>
  )
}
