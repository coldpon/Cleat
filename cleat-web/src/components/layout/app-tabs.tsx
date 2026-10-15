// 工作区标签栏(CS/FlyC2 主界面): 每个标签 = "会话 × 功能"; 点击切换 / ×关闭。
import { Activity, Camera, FolderTree, ListTree, Monitor, Settings, SquareTerminal, Terminal, X } from 'lucide-react'
import { useTabsStore, type TabKind } from '@/stores/tabs-store'
import { cn } from '@/lib/utils'

const TAB_ICONS: Record<TabKind, React.ElementType> = {
  terminal: Terminal,
  files: FolderTree,
  processes: ListTree,
  hostinfo: Monitor,
  monitor: Activity,
  screenshot: Camera,
  config: Settings,
  commands: SquareTerminal,
}

export function AppTabs() {
  const tabs = useTabsStore((s) => s.tabs)
  const activeKey = useTabsStore((s) => s.activeKey)
  const closeTab = useTabsStore((s) => s.closeTab)
  const setActive = useTabsStore((s) => s.setActive)

  if (tabs.length === 0) return null

  return (
    <div className='flex h-10 shrink-0 items-center gap-1 overflow-x-auto border-t bg-muted/40 px-2 [scrollbar-width:thin]'>
      {tabs.map((tab) => {
        const Icon = TAB_ICONS[tab.kind]
        const active = tab.key === activeKey
        return (
          <div
            key={tab.key}
            onClick={() => setActive(tab.key)}
            className={cn(
              'group/tab flex h-7 shrink-0 cursor-pointer items-center gap-1.5 rounded-md border px-2.5 font-mono text-xs font-medium transition-colors select-none',
              active
                ? 'border-border bg-background text-foreground shadow-sm'
                : 'border-transparent text-muted-foreground hover:bg-background/60 hover:text-foreground'
            )}
          >
            <Icon className={cn('size-3.5', active && 'text-primary')} />
            <span className='max-w-52 truncate'>{tab.title}</span>
            <button
              aria-label='关闭标签'
              onClick={(e) => {
                e.stopPropagation()
                closeTab(tab.key)
              }}
              className={cn(
                '-me-1 ms-0.5 rounded p-0.5 transition-opacity hover:bg-muted',
                active ? 'opacity-60 hover:opacity-100' : 'opacity-0 group-hover/tab:opacity-60'
              )}
            >
              <X className='size-3' />
            </button>
          </div>
        )
      })}
    </div>
  )
}
