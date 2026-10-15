// 工作区宿主: FlyC2 模式 —— 每个标签 = "会话 × 功能"。
// 实例保活(display 切换): 多标签的终端/文件页状态不丢。
import { useTabsStore } from '@/stores/tabs-store'
import { cn } from '@/lib/utils'
import { Terminal } from 'lucide-react'
import { TerminalPanel } from '@/features/terminal/terminal-panel'
import { FileManager } from '@/features/files/file-manager'
import { HostInfoPanel } from '@/features/sessions/host-info-panel'
import { ScreenshotPanel } from '@/features/sessions/screenshot-panel'
import { HostMgmtPanel } from '@/features/sessions/host-mgmt-panel'
import { MonitorPanel } from '@/features/sessions/monitor-panel'
import { ConfigPanel } from '@/features/sessions/config-panel'
import { CommandPanel } from '@/features/sessions/command-panel'

function TabContent({ tab }: { tab: import('@/stores/tabs-store').TabItem }) {
  switch (tab.kind) {
    case 'terminal':
      return <TerminalPanel sessionId={tab.sessionId} />
    case 'files':
      return <FileManager sessionId={tab.sessionId} />
    case 'hostinfo':
      return <HostInfoPanel sessionId={tab.sessionId} />
    case 'screenshot':
      return <ScreenshotPanel sessionId={tab.sessionId} />
    case 'processes':
      return <HostMgmtPanel sessionId={tab.sessionId} />
    case 'monitor':
      return <MonitorPanel sessionId={tab.sessionId} />
    case 'config':
      return <ConfigPanel sessionId={tab.sessionId} />
    case 'commands':
      return <CommandPanel sessionId={tab.sessionId} />
    default:
      return null
  }
}

export function TabHost() {
  const tabs = useTabsStore((s) => s.tabs)
  const activeKey = useTabsStore((s) => s.activeKey)

  if (tabs.length === 0) {
    return (
      <div className='flex h-full flex-col items-center justify-center gap-2 text-sm text-muted-foreground'>
        <Terminal className='size-6 opacity-30' />
        点击会话行打开终端 · 右键行打开更多功能
        <span className='text-xs opacity-60'>文件 / 主机管理 / 主机信息 / 监控中心 / 屏幕截图 / 命令面板 / 运行时配置</span>
      </div>
    )
  }

  return (
    <div className='relative h-full min-h-0'>
      {tabs.map((tab) => (
        <div key={tab.key} className={cn('absolute inset-0', tab.key !== activeKey && 'hidden')}>
          <TabContent tab={tab} />
        </div>
      ))}
    </div>
  )
}
