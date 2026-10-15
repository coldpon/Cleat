/**
 * 伪 JSON 配置编辑器核心逻辑：
 * - buildPseudoJson: DumpJson 结果 → 带契约注释的伪 JSON 文本（格式化即重新序列化）
 * - parsePseudoJson: JSON5 解析 + 顶层契约校验 + 域内字段校验（error 拦截 / warning 提示）
 * - sanitizeForSubmit: 剔除只读键（_startup/_contract/recovery.key）+ channels 空链与未变链剔除
 * - diffConfig: 树级变更对比（变更预览面板）
 * - findFieldLine: 校验错误 → 文档行号（lint 诊断定位，best-effort）
 *
 * 契约对齐见 agentConfigSchema.ts 头注释（规则来源 = Agent ApplyXxx， 核对）。
 */
import JSON5 from 'json5'
import {
  FALLBACK_CONTRACT, DOMAIN_FIELDS, validateByRule, validateChannels,
  CHANNEL_FIELD_ORDER, PLATFORM_FIELD_ORDER, LIMIT_RULE_FIELD_ORDER,
  CLIPBOARD_RULE_FIELD_ORDER, FILEMON_RULE_FIELD_ORDER,
  type ContractKeySpec, type ValidationIssue,
} from '@/lib/agent-config-schema'

/** 编辑器处理的顶层只读元数据键（不渲染、不提交） */
export const READONLY_TOP_KEYS = ['_startup', '_contract'] as const

export type JsonObject = Record<string, unknown>

// ============ 伪 JSON 生成 ============

/** 标量安全序列化：JSON 字符串语法是 JSON5 子集，直接复用 JSON.stringify */
function scalar(v: unknown): string {
  if (v === null) return 'null'
  if (typeof v === 'string') return JSON.stringify(v)
  return String(v)
}

const pad = (n: number) => '  '.repeat(n)

/**
 * 嵌套字段顺序表：按"路径形状"选择对象键的输出顺序。
 * `[]` 后缀表示数组元素。未命中路径的对象按已有键序输出（前瞻兼容）。
 */
const NESTED_ORDERS: { pattern: RegExp; order: readonly string[] }[] = [
  { pattern: /^channels\[\]$/, order: CHANNEL_FIELD_ORDER },
  { pattern: /^recovery\.platforms\[\]$/, order: PLATFORM_FIELD_ORDER },
  { pattern: /^channel_limit\.commands\.[^.]+$/, order: LIMIT_RULE_FIELD_ORDER },
  { pattern: /^channel_limit\.default$/, order: LIMIT_RULE_FIELD_ORDER },
  { pattern: /^clipboard\.rules\[\]$/, order: CLIPBOARD_RULE_FIELD_ORDER },
  { pattern: /^file_monitor\.rules\[\]$/, order: FILEMON_RULE_FIELD_ORDER },
]

function orderedKeys(path: string, obj: JsonObject): string[] {
  for (const { pattern, order } of NESTED_ORDERS) {
    if (pattern.test(path)) {
      const known = order.filter(k => k in obj)
      const rest = Object.keys(obj).filter(k => !order.includes(k))
      return [...known, ...rest]
    }
  }
  const domain = path.replace(/\[\]$/, '')
  if (DOMAIN_FIELDS[domain] && Object.keys(DOMAIN_FIELDS[domain]).length > 0) {
    const order = Object.keys(DOMAIN_FIELDS[domain])
    const known = order.filter(k => k in obj)
    const rest = Object.keys(obj).filter(k => !order.includes(k))
    return [...known, ...rest]
  }
  return Object.keys(obj)
}

/** 紧凑单行长度阈值：嵌套对象/数组超过则换多行展开 */
const COMPACT_LIMIT = 96

/**
 * 紧凑单行序列化（带空格的舒适形态）：`{ "type": "https", "port": 443 }` / `[1, 2]`。
 * 递归按契约顺序排键；空对象/数组输出 {} / []。
 */
function compact(v: unknown, path: string): string {
  if (v === null || typeof v !== 'object') return scalar(v)
  if (Array.isArray(v)) return `[${v.map(item => compact(item, `${path}[]`)).join(', ')}]`
  const obj = v as JsonObject
  const body = orderedKeys(path, obj)
    .map(k => `"${k}": ${compact(obj[k], path ? `${path}.${k}` : k)}`)
    .join(', ')
  return body ? `{ ${body} }` : '{}'
}

/**
 * 值序列化：返回行数组。
 * - 单行时 = [text]（无缩进，由调用方拼 `"key": ` 前缀）
 * - 多行时 = ['{', 内部行(自带完整缩进), '}']，首尾行不带缩进（调用方加）
 * - 无尾逗号（JSON 风格，比 JSON5 尾逗号流整齐）
 * - 策略：顶层域对象(indent===1)总多行；其余先试紧凑单行，超阈值展开
 */
function serializeValue(v: unknown, path: string, indent: number): string[] {
  if (v === null || typeof v !== 'object') return [scalar(v)]

  const isEmptyContainer
    = (Array.isArray(v) && v.length === 0) || (!Array.isArray(v) && Object.keys(v as JsonObject).length === 0)
  if (indent > 1 || isEmptyContainer) {
    const c = compact(v, path)
    if (c.length <= COMPACT_LIMIT) return [c]
  }

  if (Array.isArray(v)) {
    if (v.length === 0) return ['[]']
    const lines: string[] = ['[']
    v.forEach((item, i) => {
      const comma = i === v.length - 1 ? '' : ','
      const child = serializeValue(item, `${path}[]`, indent + 1)
      if (child.length === 1) {
        lines.push(`${pad(indent + 1)}${child[0]}${comma}`)
      } else {
        lines.push(`${pad(indent + 1)}${child[0]}`)
        for (let k = 1; k < child.length - 1; k++) lines.push(child[k])
        lines.push(`${pad(indent + 1)}${child[child.length - 1]}${comma}`)
      }
    })
    lines.push(']')
    return lines
  }

  const obj = v as JsonObject
  const keys = orderedKeys(path, obj)
  if (keys.length === 0) return ['{}']
  const lines: string[] = ['{']
  keys.forEach((k, i) => {
    const comma = i === keys.length - 1 ? '' : ','
    const child = serializeValue(obj[k], path ? `${path}.${k}` : k, indent + 1)
    if (child.length === 1) {
      lines.push(`${pad(indent + 1)}"${k}": ${child[0]}${comma}`)
    } else {
      lines.push(`${pad(indent + 1)}"${k}": ${child[0]}`)
      for (let j = 1; j < child.length - 1; j++) lines.push(child[j])
      lines.push(`${pad(indent + 1)}${child[child.length - 1]}${comma}`)
    }
  })
  lines.push('}')
  return lines
}

/** 域块推送：注释行 + `"name": <值>`（serializeValue 的多行首尾行在此加缩进/逗号） */
function emitBlock(lines: string[], name: string, v: unknown, notes: string[], indent: number): void {
  for (const n of notes) lines.push(`${pad(indent)}// ${n}`)
  const body = serializeValue(v, name, indent)
  if (body.length === 1) {
    lines.push(`${pad(indent)}"${name}": ${body[0]},`)
  } else {
    lines.push(`${pad(indent)}"${name}": ${body[0]}`)
    for (let i = 1; i < body.length - 1; i++) lines.push(body[i])
    lines.push(`${pad(indent)}${body[body.length - 1]},`)
  }
}

/**
 * DumpJson 结果 → 伪 JSON 文本。
 * @param config   GetConfig 返回的 config（可含 _startup/_contract，自动跳过）
 * @param contract 顶层契约（决定域顺序与注释；缺省用本地兜底表）
 */
export function buildPseudoJson(config: JsonObject, contract?: ContractKeySpec[]): string {
  const keys = contract ?? FALLBACK_CONTRACT
  const lines: string[] = ['{']
  const emitted = new Set<string>()

  for (const spec of keys) {
    if (!(spec.name in config)) continue
    emitted.add(spec.name)
    const notes: string[] = [spec.note]
    // channels：示例注释**常驻**（peng ：不管有没有数据都显示，方便参照）；
    // 空链额外带 ⚠ 提示（无法区分"未下发"与"解封失败"，仅在 JSON 内注释，不占界面空间）
    if (spec.name === 'channels') {
      if (Array.isArray(config[spec.name]) && (config[spec.name] as unknown[]).length === 0) {
        notes.push('⚠ 链为空：Agent 使用启动装配链，无法区分是未下发还是解封失败。如需下发，请填入完整通道链（空数组会被 Agent 拒绝）。')
      }
      // 示例改为"纯值形态"（不带 "channels": 壳）：直接整体替换下面 channels 的当前值即可套用。
      // 旧示例带 `"channels": [` 壳 + "去掉行首 // 即可套用" 指引 —— 照做会同时留下示例与
      // 真值两处 channels 键、且中间缺逗号 → JSON5 语法错误（实测复现，即"示例标红"根因）。
      notes.push('示例（数组顺序即优先级，可混写多个类型；复制下面数组、去掉行首 "//"，替换进 channels 当前值即可套用）：')
      notes.push('[')
      notes.push('  { "type": "https", "host": "10.0.0.1", "port": 18443, "url": "/index.php" },')
      notes.push('  { "type": "wss", "url": "wss://10.0.0.1:18443/ws" },')
      notes.push('  { "type": "quic", "host": "10.0.0.1", "port": 4433 },')
      notes.push('  { "type": "tcp", "host": "10.0.0.2", "port": 4444 },')
      notes.push('  { "type": "dns", "dns_server": "1.2.3.4:53", "dns_domain": "c2.example.com" },')
      notes.push('  { "type": "icmp", "icmp_target": "10.0.0.3" }')
      notes.push(']')
    }
    emitBlock(lines, spec.name, config[spec.name], notes, 1)
  }

  // config 中存在但契约未声明的域（前瞻兼容：不丢数据，标注未知）
  for (const k of Object.keys(config)) {
    if (emitted.has(k) || (READONLY_TOP_KEYS as readonly string[]).includes(k)) continue
    emitBlock(lines, k, config[k], ['（未知域：当前契约未声明，Agent 可能忽略）'], 1)
  }

  // 收尾行去逗号（跳过注释/空行，找到最后一个实体行）
  for (let i = lines.length - 1; i >= 0; i--) {
    const t = lines[i].trim()
    if (t === '' || t.startsWith('//')) continue
    if (lines[i].endsWith(',')) lines[i] = lines[i].slice(0, -1)
    break
  }
  lines.push('}')
  return lines.join('\n')
}

// ============ 解析与校验 ============

export interface ParsedDoc {
  ok: boolean
  data?: JsonObject
  issues: ValidationIssue[]
  parseError?: { message: string; line: number; column: number }
}

/** 从 JSON5 错误消息提取行列（json5@2 格式 "at 3:5"），提取不到挂第 1 行 */
function extractPos(e: unknown): { line: number; column: number; message: string } {
  const msg = e instanceof Error ? e.message : String(e)
  const m = msg.match(/at (\d+):(\d+)/)
  if (m) return { line: Number(m[1]), column: Number(m[2]), message: msg.replace(/^JSON5: /, '') }
  return { line: 1, column: 1, message: msg }
}

/** 校验文档：解析 + 契约校验 + 域内校验；错误行号由 findFieldLine 延迟回填 */
export function parsePseudoJson(text: string, contract?: ContractKeySpec[]): ParsedDoc {
  let parsed: unknown
  try {
    parsed = JSON5.parse(text)
  } catch (e) {
    const pos = extractPos(e)
    return { ok: false, issues: [], parseError: pos }
  }
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return { ok: false, issues: [{ path: '(root)', severity: 'error', message: `顶层必须是 JSON 对象，实际为 ${Array.isArray(parsed) ? 'array' : String(parsed)}` }] }
  }
  const data = parsed as JsonObject
  const keys = contract ?? FALLBACK_CONTRACT
  const issues: ValidationIssue[] = []
  const contractNames = new Set(keys.map(k => k.name))

  // 1. 顶层契约校验
  for (const spec of keys) {
    if (!(spec.name in data)) continue
    const v = data[spec.name]
    if (spec.name === 'channels') {
      // channels 特例：契约 kind=domain，实际形态为数组，单独校验
      if (!Array.isArray(v)) {
        issues.push({ path: 'channels', severity: 'error', message: `channels 必须是数组，实际为 ${typeof v}` })
      } else {
        issues.push(...validateChannels(v))
      }
      continue
    }
    if (spec.kind === 'domain' && (typeof v !== 'object' || v === null || Array.isArray(v))) {
      issues.push({ path: spec.name, severity: 'error', message: `域 ${spec.name} 必须是对象，实际为 ${Array.isArray(v) ? 'array' : typeof v}` })
    }
  }

  // 2. 只读键检查（提交时会被剔除，这里提示）
  for (const ro of READONLY_TOP_KEYS) {
    if (ro in data) issues.push({ path: ro, severity: 'warning', message: `${ro} 为只读元数据，提交时将被剔除` })
  }
  if (data.recovery && typeof data.recovery === 'object' && !Array.isArray(data.recovery)
      && 'key' in (data.recovery as JsonObject)) {
    issues.push({ path: 'recovery.key', severity: 'warning', message: 'recovery.key 敏感不回读，提交时会剔除该键（设置请走 CLI）' })
  }

  // 3. 未知顶层键
  for (const k of Object.keys(data)) {
    if (!contractNames.has(k)) {
      issues.push({ path: k, severity: 'warning', message: '未知顶层键：当前契约未声明，Agent 将忽略' })
    }
  }

  // 4. 域内字段校验
  for (const [domain, fields] of Object.entries(DOMAIN_FIELDS)) {
    const dv = data[domain]
    if (dv === null || typeof dv !== 'object' || Array.isArray(dv)) continue
    const dObj = dv as JsonObject
    for (const [field, spec] of Object.entries(fields)) {
      if (!(field in dObj)) continue
      issues.push(...validateByRule(`${domain}.${field}`, dObj[field], spec))
    }
    for (const k of Object.keys(dObj)) {
      if (!(k in fields)) {
        issues.push({ path: `${domain}.${k}`, severity: 'warning', message: '未知字段：Agent 将忽略' })
      }
    }
  }

  const hasError = issues.some(i => i.severity === 'error')
  return { ok: !hasError, data, issues }
}

// ============ 提交净化 ============

export interface SanitizeResult {
  payload: JsonObject
  removedKeys: string[]
  /** channels 是否被剔除（未变/空链保持） */
  channelsOmitted: boolean
}

function deepEqual(a: unknown, b: unknown): boolean {
  return JSON.stringify(a) === JSON.stringify(b)
}

/** 仅剔除只读键（_startup/_contract/recovery.key），不动 channels —— diff 双侧对称化用 */
export function stripReadonly(obj: JsonObject): JsonObject {
  const out: JsonObject = { ...obj }
  for (const ro of READONLY_TOP_KEYS) delete out[ro]
  if (out.recovery && typeof out.recovery === 'object' && !Array.isArray(out.recovery)
      && 'key' in (out.recovery as JsonObject)) {
    const rec = { ...(out.recovery as JsonObject) }
    delete rec.key
    out.recovery = rec
  }
  return out
}

/**
 * 提交前净化：
 * - 剔除 _startup / _contract / recovery.key（只读元数据 / 敏感不回读）
 * - channels：**非空链永远全量重发**（peng 裁决 ：通道链 Agent 侧原子整链替换，
 *   发部分数组 = 覆盖掉其余通道；前端的"未变更剔除"类优化在整链语义下不做，
 *   避免任何"该发的没发"的窗口）。唯一特例：回读空链（未下发状态）且操作员未编辑
 *   时剔除该键（需求）—— 空数组下发会被 Agent 拒绝，回执打 error 殃及其他域。
 * - 其余域全量提交（Agent 语义：域内字段级合并，未携带字段/域不动）
 */
export function sanitizeForSubmit(data: JsonObject, loadedConfig: JsonObject | null): SanitizeResult {
  const payload = stripReadonly(data)
  const removedKeys: string[] = []
  // 报告哪些只读键真的被剔除（正常编辑流程不出现，防御性剔除时 UI 有感知）
  if ('_startup' in data) removedKeys.push('_startup')
  if ('_contract' in data) removedKeys.push('_contract')
  if (data.recovery && typeof data.recovery === 'object' && !Array.isArray(data.recovery)
      && 'key' in (data.recovery as JsonObject)) removedKeys.push('recovery.key')

  let channelsOmitted = false
  if ('channels' in payload) {
    const cur = payload.channels
    const loaded = loadedConfig && typeof loadedConfig === 'object' ? (loadedConfig as JsonObject).channels : undefined
    // 空链未编辑：保持未下发状态（Agent 拒绝空数组，下发只会打 error）
    if (Array.isArray(cur) && cur.length === 0 && Array.isArray(loaded) && loaded.length === 0) {
      delete payload.channels
      channelsOmitted = true
      removedKeys.push('channels(空链保持, Agent 使用启动装配链)')
    } else if (Array.isArray(cur) && cur.length === 0) {
      // 原链非空但编辑成了空数组：校验层已提示（warning, 不拦截, 见 validateChannels）；
      // 这里是双保险，确保空数组永远到不了 Agent（会打 error 殃及其他域）
      delete payload.channels
      channelsOmitted = true
      removedKeys.push('channels(空数组已拦截)')
    }
    // 其余情况：非空链全量重发（含与回读相同的链 —— 显式重 seal 由操作员意图兜底）
  }
  return { payload, removedKeys, channelsOmitted }
}

// ============ 变更 diff ============

export interface DiffEntry {
  path: string
  type: 'changed' | 'added' | 'removed'
  oldValue?: string
  newValue?: string
}

function fmt(v: unknown): string {
  if (v === undefined) return '∅'
  const s = JSON.stringify(v)
  if (s === undefined) return String(v)
  return s.length > 160 ? `${s.slice(0, 157)}…` : s
}

/**
 * 两棵配置树对比（sanitize 后的契约域对象）。
 * 数组元素逐索引对比（尾部增删记 added/removed）；对象元素下钻一层，标量直接对比。
 */
export function diffConfig(original: JsonObject, current: JsonObject): DiffEntry[] {
  const out: DiffEntry[] = []
  const domains = new Set([...Object.keys(original), ...Object.keys(current)])
  for (const d of domains) {
    if ((READONLY_TOP_KEYS as readonly string[]).includes(d)) continue
    walk(d, original[d], current[d], out)
  }
  return out
}

function walk(path: string, a: unknown, b: unknown, out: DiffEntry[]): void {
  if (deepEqual(a, b)) return
  if (Array.isArray(a) && Array.isArray(b)) {
    const n = Math.max(a.length, b.length)
    for (let i = 0; i < n; i++) {
      const p = `${path}[${i}]`
      if (i >= a.length) { out.push({ path: p, type: 'added', newValue: fmt(b[i]) }); continue }
      if (i >= b.length) { out.push({ path: p, type: 'removed', oldValue: fmt(a[i]) }); continue }
      if (deepEqual(a[i], b[i])) continue
      if (a[i] !== null && b[i] !== null && typeof a[i] === 'object' && typeof b[i] === 'object') {
        walk(p, a[i], b[i], out)
      } else {
        out.push({ path: p, type: 'changed', oldValue: fmt(a[i]), newValue: fmt(b[i]) })
      }
    }
    return
  }
  if (a !== null && b !== null && typeof a === 'object' && typeof b === 'object') {
    const ao = a as JsonObject
    const bo = b as JsonObject
    const ks = new Set([...Object.keys(ao), ...Object.keys(bo)])
    for (const k of ks) {
      const p = `${path}.${k}`
      if (!(k in bo)) { out.push({ path: p, type: 'removed', oldValue: fmt(ao[k]) }); continue }
      if (!(k in ao)) { out.push({ path: p, type: 'added', newValue: fmt(bo[k]) }); continue }
      walk(p, ao[k], bo[k], out)
    }
    return
  }
  out.push({ path, type: 'changed', oldValue: fmt(a), newValue: fmt(b) })
}

// ============ 行定位（lint 诊断，best-effort） ============

/**
 * 在伪 JSON 文本中定位字段路径的行号（1-based）。
 * 策略：逐段缩小窗口 —— 顶层域找 `"domain":` 行并收窄到下一个顶层域；
 * 数组索引段在窗口内做字符串感知的括号深度扫描，找第 i 个元素对象；
 * 普通段找 `"field":` 行。任何一段找不到 → undefined（诊断挂第 1 行）。
 */
export function findFieldLine(text: string, path: string): number | undefined {
  const lines = text.split('\n')
  const segs: { key: string; index?: number }[] = []
  for (const part of path.split('.')) {
    const m = part.match(/^([^[.]+)(?:\[(\d+)\])?$/)
    if (!m) return undefined
    segs.push({ key: m[1], index: m[2] !== undefined ? Number(m[2]) : undefined })
  }

  let winStart = 0
  let winEnd = lines.length

  const findKeyLine = (key: string, from: number, to: number): number => {
    const re = new RegExp(`"${key.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}"\\s*:`)
    for (let i = from; i < to; i++) {
      if (re.test(lines[i])) return i
    }
    return -1
  }

  for (let s = 0; s < segs.length; s++) {
    const seg = segs[s]
    const keyLine = findKeyLine(seg.key, winStart, winEnd)
    if (keyLine < 0) return undefined

    if (s === 0) {
      // 顶层域：窗口 = [keyLine, 下一个顶层域键行)；keyLine 即当前段定位结果
      let next = lines.length
      for (let i = keyLine + 1; i < lines.length; i++) {
        if (/^ {2}"[A-Za-z_]+":/.test(lines[i])) { next = i; break }
      }
      winStart = keyLine
      winEnd = next
    } else {
      winStart = keyLine
    }

    if (seg.index !== undefined) {
      const elemLine = findArrayElement(lines, winStart, winEnd, seg.index)
      if (elemLine < 0) return undefined
      winStart = elemLine
      if (s === segs.length - 1) return elemLine + 1
    } else if (s === segs.length - 1) {
      return keyLine + 1
    }
  }
  return undefined
}

/** 字符串感知扫描：在 [from, to) 内找第 want 个对象元素的起始行（best-effort） */
function findArrayElement(lines: string[], from: number, to: number, want: number): number {
  let seen = -1
  for (let i = from; i < to; i++) {
    const line = lines[i]
    let inStr = false
    let esc = false
    for (const ch of line) {
      if (esc) { esc = false; continue }
      if (inStr) {
        if (ch === '\\') esc = true
        else if (ch === '"') inStr = false
        continue
      }
      if (ch === '"') { inStr = true; continue }
      if (ch === '{') {
        seen++
        if (seen === want) return i
      }
    }
  }
  return -1
}
