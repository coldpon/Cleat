// Cleat 系统域 REST: 凭据/目标/监听器/隧道/操作员/审计/载荷/模块。
// 端点与类型对齐 cleat-web/src/api/cleat/*; 复用 cleat-api 的 api 封装(Bearer/401/错误解析)。
import { api, getToken, postBinary } from './cleat-api'

// ---- 凭据 ----
export interface Credential {
  id: number
  username: string
  realm: string
  cred_type: 'password' | 'hash' | 'token' | 'ticket' | 'key' | 'other'
  secret: string
  host: string
  source: string
  agent_id: number
  tag: string
  created_at: number
}

export function listCredentials() {
  return api.get<{ credentials: Credential[] }>('/api/credentials')
}

export function addCredential(data: Partial<Omit<Credential, 'id' | 'created_at'>>) {
  return api.post<{ status: string; id: number }>('/api/credentials', data)
}

export function deleteCredential(id: number) {
  return api.del<{ status: string }>(`/api/credentials/${id}`)
}

// ---- 目标 ----
export type TargetStatus = 'new' | 'scanned' | 'compromised' | 'closed'

export interface TargetRow {
  id: number
  ip: string
  hostname: string
  os: string
  ports: string
  protocol: string
  status: TargetStatus
  tags: string
  notes: string
  agent_id: number
  created_at: number
  updated_at: number
}

export function listTargets() {
  return api.get<{ targets: TargetRow[] }>('/api/targets')
}

export function addTarget(
  data: { ip: string } & Partial<Omit<TargetRow, 'id' | 'ip' | 'created_at' | 'updated_at'>>
) {
  return api.post<{ status: string; id: number }>('/api/targets', data)
}

export function updateTarget(id: number, data: Partial<Omit<TargetRow, 'id' | 'created_at' | 'updated_at'>>) {
  return api.put<{ status: string }>(`/api/targets/${id}`, data)
}

export function deleteTarget(id: number) {
  return api.del<{ status: string }>(`/api/targets/${id}`)
}

// ---- 监听器 ----
export interface ListenerRow {
  address: string
  port: number
  protocol: string
  status: 'running' | 'stopped'
}

export function listListeners() {
  return api.get<{ listeners: ListenerRow[] }>('/api/listeners')
}

export function createListener(data: {
  protocol?: string
  address?: string
  port?: number
  domain?: string
  ws_path?: string
}) {
  return api.post<ListenerRow>('/api/listeners', data)
}

export function deleteListener(address: string) {
  return api.del<{ ok: boolean }>(`/api/listeners/${encodeURIComponent(address)}`)
}

// ---- 隧道(SOCKS 出口) ----
export interface PivotRow {
  session_id: number
  local_addr: string
  port: number
  entry_host: string
  entry_port: number
  status: 'running' | 'stopped'
}

export function listPivots() {
  return api.get<{ pivots: PivotRow[] }>('/api/pivots')
}

export function createPivot(data: { session_id: number; port: number; entry_host?: string; entry_port?: number }) {
  return api.post<PivotRow>('/api/pivots', data)
}

export function deletePivot(localAddr: string) {
  return api.del<{ ok: boolean }>(`/api/pivots/${encodeURIComponent(localAddr)}`)
}

// ---- 反向端口转发 ----
export interface RPortFwdRow {
  session_id: number
  listen_port: number
  target_host: string
  target_port: number
}

export function listRPortFwds() {
  return api.get<{ rportfwds: RPortFwdRow[] }>('/api/rportfwds')
}

export function createRPortFwd(data: {
  session_id: number
  listen_port: number
  target_host: string
  target_port: number
}) {
  return api.post<{ agent_addr: string; target: string; status: string }>('/api/rportfwds', data)
}

export function deleteRPortFwd(sessionId: number, listenPort: number) {
  return api.del<{ ok: boolean }>(`/api/rportfwds?session_id=${sessionId}&listen_port=${listenPort}`)
}

// ---- 操作员 ----
export interface OperatorRow {
  username: string
  role: 'admin' | 'operator' | 'viewer'
  active: boolean
}

export function listOperators() {
  return api.get<{ operators: OperatorRow[] }>('/api/operators')
}

export function addOperator(data: { username: string; role: OperatorRow['role']; token: string }) {
  return api.post<{ status: string }>('/api/operators', data)
}

export function deleteOperator(username: string) {
  return api.del<{ status: string }>(`/api/operators/${encodeURIComponent(username)}`)
}

export function toggleOperator(username: string) {
  return api.post<{ status: string; active: boolean }>(`/api/operators/${encodeURIComponent(username)}/toggle`)
}

export function changeOperatorPassword(username: string, oldPassword: string, newPassword: string) {
  return api.post<{ status: string }>(`/api/operators/${encodeURIComponent(username)}/password`, {
    old_password: oldPassword,
    new_password: newPassword,
  })
}

// ---- 审计 ----
export interface AuditRow {
  id: number
  ts: number
  operator: string
  session_id: number
  cmd: string
  mode: string
  status: string
}

export function listAudit(
  q: { operator?: string; session_id?: number; cmd?: string; status?: string; limit?: number; offset?: number } = {}
) {
  const params = new URLSearchParams()
  if (q.operator) params.set('operator', q.operator)
  if (q.session_id) params.set('session_id', String(q.session_id))
  if (q.cmd) params.set('cmd', q.cmd)
  if (q.status) params.set('status', q.status)
  params.set('limit', String(q.limit ?? 200))
  params.set('offset', String(q.offset ?? 0))
  return api.get<{ items: AuditRow[]; total: number }>(`/api/audit?${params.toString()}`)
}

// ---- 载荷 ----
export interface PayloadStub {
  form: string
  file: string
  agent_commit: string
  block_version: number
  size: number
  sha256: string
  built_at: string
}

export interface PayloadEntry {
  name: string
  form: string
  size: number
  built_at: string
}

export interface PayloadResult {
  name: string
  form: string
  bin_path: string
  download_url: string
  run_line: string
  args: string[] | null
  warnings: string[] | null
  built_at: string
}

export function listPayloadStubs() {
  return api.get<{ stubs: PayloadStub[] }>('/api/payloads/stubs')
}

export function listPayloads() {
  return api.get<{ payloads: PayloadEntry[] }>('/api/payloads')
}

export function generatePayload(req: Record<string, unknown>) {
  return api.post<PayloadResult>('/api/payloads/generate', req)
}

export function deletePayload(name: string) {
  return api.del<{ ok: boolean }>(`/api/payloads/${encodeURIComponent(name)}`)
}

/** 认证下载(Bearer): 拉取二进制 → 触发浏览器保存(文件名优先取 Content-Disposition) */
export async function downloadPayload(name: string) {
  const token = getToken()
  const resp = await fetch(`/api/payloads/${encodeURIComponent(name)}/download`, {
    headers: token ? { Authorization: `Bearer ${token}` } : {},
  })
  if (!resp.ok) {
    let msg = `HTTP ${resp.status}`
    try {
      const t = await resp.text()
      if (t) msg = t
    } catch {
      /* ignore */
    }
    throw new Error(msg)
  }
  const cd = resp.headers.get('Content-Disposition') || ''
  const m = /filename="?([^";]+)"?/.exec(cd)
  const blob = await resp.blob()
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = m ? m[1] : name
  a.click()
  URL.revokeObjectURL(url)
}

// ---- 模块 ----
export interface ModuleRow {
  name: string
  size: number
  sha256: string
  modified: string
}

export function listModules() {
  return api.get<{ modules: ModuleRow[] }>('/api/modules')
}

/** 上传模块 DLL(query name, body 原始字节) */
export async function uploadModule(name: string, file: File | Blob) {
  const bytes = new Uint8Array(await file.arrayBuffer())
  return postBinary<{ name: string; size: number; stored: boolean }>(
    `/api/modules?name=${encodeURIComponent(name)}`,
    bytes,
    'application/octet-stream'
  )
}

export function deleteModule(name: string) {
  return api.del<{ deleted: boolean }>(`/api/modules/${encodeURIComponent(name)}`)
}
