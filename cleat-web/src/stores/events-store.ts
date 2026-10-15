// 全局事件流 store:
// GlobalEventsBridge(认证布局顶层)单点连接 /api/events SSE 并写入;
// 总览 LiveFeed 与 事件日志页 共用同一份数据(页面各自决定过滤/排序)。
// 新事件在队首, 容量上限 300 条; 历史事件(REST /api/events/history)启动时一次性灌入。
import { create } from 'zustand'

export type EventLevel = 'info' | 'ok' | 'warn' | 'err'

export interface GlobalEventItem {
  key: number
  level: EventLevel
  text: string
  ts: number
}

const MAX = 300

interface EventsState {
  events: GlobalEventItem[]
  /** 追加一条(新事件在队首) */
  push: (level: EventLevel, text: string, ts?: number) => void
  /** 批量灌入(参数为旧→新顺序; 存储后新事件在队首) */
  seed: (items: { level: EventLevel; text: string; ts?: number }[]) => void
  clear: () => void
}

let keySeq = 0

export const useEventsStore = create<EventsState>()((set) => ({
  events: [],
  push: (level, text, ts) =>
    set((s) => {
      const now = ts || Date.now()
      // 同文本 2s 内去重: 服务端注册/重连路径可能双发 session_online 等事件
      const top = s.events[0]
      if (top && top.text === text && Math.abs(now - top.ts) < 2000) return s
      return { events: [{ key: ++keySeq, level, text, ts: now }, ...s.events].slice(0, MAX) }
    }),
  seed: (items) =>
    set(() => ({
      events: items
        .map((it) => ({ key: ++keySeq, level: it.level, text: it.text, ts: it.ts || Date.now() }))
        .reverse()
        .slice(0, MAX),
    })),
  clear: () => set({ events: [] }),
}))
