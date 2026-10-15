/**
 * 拓扑图数据模型与分层布局（纯函数，无框架依赖）
 *
 * 图形态：左→右三列
 *   [TeamServer(+监听器)] ──C2 链路──▶ [Agent 会话] ──转发──▶ [隧道端点]
 * 隧道端点来自转发模块：SOCKS 出口 / 多跳链(chain@host:port) / 反向端口转发目标。
 * 布局按"会话块"逐行排布：每个会话块高 = max(节点高, 其隧道堆叠高)，保证不重叠。
 */
import type { SessionRow } from '@/lib/cleat-api'
import type { ListenerRow, PivotRow, RPortFwdRow } from '@/lib/cleat-rest'

// ============ 类型 ============

/** 隧道端点（转发模块成果的图上最小单元） */
export type TopoTunnel =
  | {kind: 'socks'; sessionId: number; localAddr: string; port: number}
  | {kind: 'chain'; sessionId: number; localAddr: string; port: number; entryHost: string; entryPort: number}
  | {kind: 'rportfwd'; sessionId: number; listenPort: number; targetHost: string; targetPort: number}

export type TopoNodeKind = 'server' | 'agent' | 'tunnel'

export interface TopoNode {
  id: string
  kind: TopoNodeKind
  /** kind=agent 时的会话信息 */
  session?: SessionRow
  /** kind=tunnel 时的隧道信息 */
  tunnel?: TopoTunnel
  /** kind=server 时挂载的监听器 */
  listeners?: ListenerRow[]
  x: number
  y: number
  w: number
  h: number
}

export interface TopoEdge {
  id: string
  source: string
  target: string
  kind: 'c2' | 'socks' | 'chain' | 'rportfwd'
  /** c2 边的传输通道标签(https/tcp/...) */
  label?: string
  online: boolean
  d: string // SVG path
  mx: number // 标签锚点 x
  my: number // 标签锚点 y
}

export interface TopoGraph {
  nodes: TopoNode[]
  edges: TopoEdge[]
  width: number
  height: number
}

export interface TopoInput {
  sessions: SessionRow[]
  listeners: ListenerRow[]
  pivots: PivotRow[]
  rportfwds: RPortFwdRow[]
}

// ============ 尺寸常量 ============

export const TOPO = {
  serverW: 236,
  agentW: 236,
  agentH: 88,
  tunW: 224,
  tunH: 58,
  colGap: 96,
  rowGap: 26,
  tunGap: 10,
  pad: 24,
  /** 服务端节点上最多铺开的监听器 chip 数 */
  maxChips: 3,
  chipH: 24,
} as const

/** 服务端节点高度随监听器 chip 数变化 */
function serverHeight(listenerCount: number): number {
  const chips = Math.min(listenerCount, TOPO.maxChips)
  const more = listenerCount > TOPO.maxChips ? 18 : 0
  return 66 + chips * TOPO.chipH + more
}

// ============ 工具 ============

/** 三次贝塞尔 B(0.5) = (P0 + 3P1 + 3P2 + P3) / 8，横向对称时中点即均值。
 * 控制点距离取列间距的 0.7 且下限 56：出/入边尽量水平，避免大纵向跨度时弧线扫回节点。 */
function bezier(sx: number, sy: number, tx: number, ty: number) {
  const k = Math.max(56, Math.abs(tx - sx) * 0.7)
  return {
    d: `M ${sx} ${sy} C ${sx + k} ${sy}, ${tx - k} ${ty}, ${tx} ${ty}`,
    mx: (sx + tx) / 2,
    my: (sy + ty) / 2,
  }
}

function edgePoint(node: TopoNode, side: 'out' | 'in') {
  const x = side === 'out' ? node.x + node.w : node.x
  return {x, y: node.y + node.h / 2}
}

function makeEdge(
  id: string, source: TopoNode, target: TopoNode,
  kind: TopoEdge['kind'], online: boolean, label?: string,
): TopoEdge {
  const p1 = edgePoint(source, 'out')
  const p2 = edgePoint(target, 'in')
  const {d, mx, my} = bezier(p1.x, p1.y, p2.x, p2.y)
  return {id, source: source.id, target: target.id, kind, label, online, d, mx, my}
}

// ============ 构建 + 布局 ============

export function buildTopologyGraph(input: TopoInput): TopoGraph {
  const {sessions, listeners, pivots, rportfwds} = input
  const nodes: TopoNode[] = []
  const edges: TopoEdge[] = []

  // --- 隧道按会话归组(仅保留会话仍存在的，避免悬空边) ---
  const tunnelsBySid = new Map<number, TopoTunnel[]>()
  const pushTunnel = (sid: number, t: TopoTunnel) => {
    const list = tunnelsBySid.get(sid) ?? []
    list.push(t)
    tunnelsBySid.set(sid, list)
  }
  for (const p of pivots) {
    if (p.status !== 'running') continue
    const multi = !!p.entry_host && !!p.entry_port
    pushTunnel(p.session_id, multi
      ? {kind: 'chain', sessionId: p.session_id, localAddr: p.local_addr, port: p.port, entryHost: p.entry_host, entryPort: p.entry_port}
      : {kind: 'socks', sessionId: p.session_id, localAddr: p.local_addr, port: p.port})
  }
  for (const r of rportfwds) {
    pushTunnel(r.session_id, {kind: 'rportfwd', sessionId: r.session_id, listenPort: r.listen_port, targetHost: r.target_host, targetPort: r.target_port})
  }

  // --- 会话节点：在线优先，同状态按 ID 升序 ---
  const ordered = [...sessions].sort((a, b) => (a.online === b.online ? a.id - b.id : a.online ? -1 : 1))

  // 逐块排布：块高 = max(节点高, 隧道堆叠高)
  const blocks = ordered.map((s) => {
    const tuns = tunnelsBySid.get(s.id) ?? []
    const tunBlockH = tuns.length ? tuns.length * TOPO.tunH + (tuns.length - 1) * TOPO.tunGap : 0
    return {s, tuns, blockH: Math.max(TOPO.agentH, tunBlockH), tunBlockH}
  })

  // 三列横坐标：server → agent → tunnel
  const layoutServerX = TOPO.pad
  const layoutAgentX = layoutServerX + TOPO.serverW + TOPO.colGap
  const layoutTunX = layoutAgentX + TOPO.agentW + TOPO.colGap

  const contentH = blocks.reduce((acc, b) => acc + b.blockH, 0) + Math.max(0, blocks.length - 1) * TOPO.rowGap
  const serverH = serverHeight(listeners.length)
  const height = Math.max(contentH, serverH) + TOPO.pad * 2

  const serverNode: TopoNode = {
    id: 'server',
    kind: 'server',
    listeners,
    x: layoutServerX,
    y: TOPO.pad + (Math.max(contentH, serverH) - serverH) / 2,
    w: TOPO.serverW,
    h: serverH,
  }
  nodes.push(serverNode)

  let cursor = TOPO.pad
  for (const b of blocks) {
    const agentY = cursor + (b.blockH - TOPO.agentH) / 2
    const agentNode: TopoNode = {
      id: `agent-${b.s.id}`,
      kind: 'agent',
      session: b.s,
      x: layoutAgentX,
      y: agentY,
      w: TOPO.agentW,
      h: TOPO.agentH,
    }
    nodes.push(agentNode)
    edges.push(makeEdge(
      `c2-${b.s.id}`, serverNode, agentNode, 'c2',
      b.s.online, (b.s.transport || '未知').toUpperCase(),
    ))

    let tunY = cursor + (b.blockH - b.tunBlockH) / 2
    for (const t of b.tuns) {
      const tunNode: TopoNode = {
        id: `tun-${t.kind}-${t.sessionId}-${t.kind === 'rportfwd' ? t.listenPort : t.port}`,
        kind: 'tunnel',
        tunnel: t,
        x: layoutTunX,
        y: tunY,
        w: TOPO.tunW,
        h: TOPO.tunH,
      }
      nodes.push(tunNode)
      edges.push(makeEdge(`${tunNode.id}-edge`, agentNode, tunNode, t.kind, b.s.online))
      tunY += TOPO.tunH + TOPO.tunGap
    }
    cursor += b.blockH + TOPO.rowGap
  }

  // 内容高度按最右列收口
  const width = layoutTunX + TOPO.tunW + TOPO.pad
  return {nodes, edges, width, height}
}

// ============ 文案 ============

export const TUNNEL_LABEL: Record<TopoTunnel['kind'], string> = {
  socks: 'SOCKS5',
  chain: '多跳',
  rportfwd: '转发',
}

/** 隧道端点主标题(节点第一行) */
export function tunnelTitle(t: TopoTunnel): string {
  switch (t.kind) {
    case 'socks':
      return t.localAddr
    case 'chain':
      return `${t.entryHost}:${t.entryPort}`
    case 'rportfwd':
      return `${t.targetHost}:${t.targetPort}`
  }
}

/** 隧道端点副标题(节点第二行) */
export function tunnelCaption(t: TopoTunnel): string {
  switch (t.kind) {
    case 'socks':
      return 'TS 本地出口 · 流量经会话出内网'
    case 'chain':
      return `出口 ${t.localAddr} 经会话链至下一跳`
    case 'rportfwd':
      return `受害机 :${t.listenPort} 入站 · 经 TS 拨出`
  }
}

/** 隧道端点详情字段(详情抽屉) */
export function tunnelDetails(t: TopoTunnel): Array<[string, string]> {
  switch (t.kind) {
    case 'socks':
      return [
        ['类型', 'SOCKS5 出口'],
        ['本地地址', t.localAddr],
        ['出会话', `#${t.sessionId}`],
      ]
    case 'chain':
      return [
        ['类型', '多跳链(SOCKS 链式)'],
        ['本地地址', t.localAddr],
        ['下一跳入口', `${t.entryHost}:${t.entryPort}`],
        ['出会话', `#${t.sessionId}`],
      ]
    case 'rportfwd':
      return [
        ['类型', '反向端口转发'],
        ['受害机监听', `:${t.listenPort}`],
        ['转发目标', `${t.targetHost}:${t.targetPort}`],
        ['入会话', `#${t.sessionId}`],
      ]
  }
}

/** 超长文本截断(SVG text 无自动省略) */
export function fitText(s: string, max: number): string {
  return s.length > max ? `${s.slice(0, max - 1)}…` : s
}
