// 工作区标签 store: FlyC2 模式 —— 每个标签 = "会话 × 功能"
// (如: 终端·kali / 文件·DESKTOP / 主机信息·WIN-SERVER01), 不再有"会话内二级 tabs"。
// 行点击默认开终端; 行右键菜单开任意功能。系统页(监听器/凭据等)走路由, 不开标签。
import { create } from 'zustand'

export type TabKind =
  | 'terminal'
  | 'files'
  | 'processes'
  | 'hostinfo'
  | 'monitor'
  | 'screenshot'
  | 'config'
  | 'commands'

export interface TabItem {
  /** 唯一键: s:<会话id>:<功能> */
  key: string
  kind: TabKind
  /** 标签标题(功能 · 主机名) */
  title: string
  sessionId: number
}

export const FN_TITLES: Record<TabKind, string> = {
  terminal: '终端',
  files: '文件',
  processes: '主机管理',
  hostinfo: '主机信息',
  monitor: '监控中心',
  screenshot: '屏幕截图',
  config: '运行时配置',
  commands: '命令面板',
}

/** 打开一个"会话 × 功能"标签(已存在则激活) */
export function sessionFnTab(id: number, hostname: string, kind: TabKind): TabItem {
  const name = hostname || `#${id}`
  return { key: `s:${id}:${kind}`, kind, sessionId: id, title: `${FN_TITLES[kind]} · ${name}` }
}

interface TabsState {
  tabs: TabItem[]
  activeKey: string
  openTab: (tab: TabItem) => void
  closeTab: (key: string) => void
  setActive: (key: string) => void
}

export const useTabsStore = create<TabsState>()((set, get) => ({
  tabs: [],
  activeKey: '',
  openTab: (tab) => {
    const { tabs, activeKey } = get()
    const exists = tabs.find((t) => t.key === tab.key)
    if (exists && activeKey === tab.key) return
    set({ tabs: exists ? tabs : [...tabs, tab], activeKey: tab.key })
  },
  closeTab: (key) => {
    const { tabs, activeKey } = get()
    const idx = tabs.findIndex((t) => t.key === key)
    if (idx < 0) return
    const nextTabs = tabs.filter((t) => t.key !== key)
    if (activeKey === key) {
      const neighbor = nextTabs[Math.max(0, idx - 1)]
      set({ tabs: nextTabs, activeKey: neighbor ? neighbor.key : '' })
    } else {
      set({ tabs: nextTabs })
    }
  },
  setActive: (key) => set({ activeKey: key }),
}))
