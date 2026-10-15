import { createFileRoute } from '@tanstack/react-router'
import { AvMonitor } from '@/features/av-monitor'
import type { StreamKind } from '@/features/stream/stream-content'

// 音视频监控页(作战导航): /av-monitor
// search 参数: ?session=<会话id>&kind=<desktop|camera|audio>
// 带参时直接进入该会话的单路大画面(会话右键菜单跳转入口); 无参时为监控墙。
const STREAM_KINDS = ['desktop', 'camera', 'audio'] as const

export const Route = createFileRoute('/_authenticated/av-monitor/')({
  validateSearch: (search: Record<string, unknown>): { session?: number; kind?: StreamKind } => {
    const out: { session?: number; kind?: StreamKind } = {}
    const id = Number(search.session)
    if (Number.isInteger(id) && id > 0) out.session = id
    const kind = STREAM_KINDS.find((k) => k === search.kind)
    if (kind) out.kind = kind
    return out
  },
  component: AvMonitorPage,
})

function AvMonitorPage() {
  const { session, kind } = Route.useSearch()
  return <AvMonitor sessionId={session} kind={kind} />
}
